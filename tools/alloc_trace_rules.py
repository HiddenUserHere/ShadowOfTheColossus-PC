#!/usr/bin/env python3
"""Find the value that separates GPU-written allocations at a mixed allocation site.

Input: the table SOTC_ALLOC_TRACE writes (default user/alloc_trace.tsv; see gpu/sotc/alloc_sites.h). One row per
traced allocation: the caller chain (c1, c2, c3), address, size, flags (bit 1 = a written buffer bind started in it,
bit 8 = a render target / storage image; bit 64 = the written bind was the allocation's own range, bit 128 = it
started at another offset or reached past the end), done (1 = the allocation was replaced by a later one, 2 = it was freed;
either way its flags are final), then the 16 registers at the sub_12820 call, 64 stack qwords from rsp, and 16 qwords behind each of rbx,
rsi, rdi, rbp, r12-r15.

For each site (c1, c2) the rows are split into GPU-written and not GPU-written. A row counts as not GPU-written when
it was freed or replaced by a later allocation (done = 1 or 2), or when it is still live with no GPU write after --min-age ms
(age_ms column; long-lived buffers such as static models are rarely freed in a short run). Younger live rows with no
GPU flag are left out (the GPU may still write them). Then every column is tested as a separator:
- value: the column takes few distinct values (8 or fewer per class) and the two classes share none;
- bit: one bit is set in every row of one class and clear in every row of the other;
- threshold: every value of one class is below every value of the other (for small integers, e.g. counts, sizes).
Pointer-like columns (values that differ in nearly every row) are skipped for "value", since unique values separate
trivially. The output lists the separators found per site, best first.

usage: python tools/alloc_trace_rules.py [user/alloc_trace.tsv] [--min-rows N]
"""
import argparse
import collections
import csv
import sys

WRITTEN = 1 | 8


def load(path):
    with open(path, newline='') as f:
        reader = csv.reader(f, delimiter='\t')
        header = next(reader)
        rows = [r for r in reader if r]
    return header, rows


def to_int(s):
    try:
        return int(s, 0)
    except ValueError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path', nargs='?', default='user/alloc_trace.tsv')
    ap.add_argument('--min-rows', type=int, default=4)
    ap.add_argument('--min-age', type=int, default=30000, help='ms a live untouched row must have lived')
    args = ap.parse_args()
    header, rows = load(args.path)
    col = {name: i for i, name in enumerate(header)}
    first_feature = col['done'] + 1
    features = ['size'] + header[first_feature:]

    sites = collections.defaultdict(list)
    for r in rows:
        sites[(r[col['c1']], r[col['c2']])].append(r)

    for (c1, c2), rs in sorted(sites.items()):
        written = [r for r in rs if int(r[col['flags']]) & WRITTEN]
        def never(r):
            if int(r[col['flags']]) & WRITTEN:
                return False
            if r[col['done']] in ('1', '2'):
                return True
            return 'age_ms' in col and int(r[col['age_ms']]) >= args.min_age
        clean = [r for r in rs if never(r)]
        pending = len(rs) - len(written) - len(clean)
        print(f'site c1={c1} c2={c2}: {len(rs)} rows, {len(written)} GPU-written, {len(clean)} never written '
              f'(replaced, or untouched for {args.min_age} ms), {pending} younger and untouched (left out)')
        if len(written) < 1 or len(clean) < 1 or len(written) + len(clean) < args.min_rows:
            print('  not enough rows of both classes\n')
            continue
        found = []
        for name in features:
            i = col[name]
            a = [to_int(r[i]) for r in written]
            b = [to_int(r[i]) for r in clean]
            if None in a or None in b:
                continue
            sa, sb = set(a), set(b)
            unique_ratio = (len(sa) + len(sb)) / (len(a) + len(b))
            if not sa & sb and len(sa) <= 8 and len(sb) <= 8 and unique_ratio < 0.5:
                found.append((0, f'value  {name}: written {sorted(hex(v) for v in sa)} vs never {sorted(hex(v) for v in sb)}'))
                continue
            for bit in range(64):
                m = 1 << bit
                if all(v & m for v in a) and not any(v & m for v in b):
                    found.append((1, f'bit    {name} bit {bit} (0x{m:x}) set when written, clear otherwise'))
                elif not any(v & m for v in a) and all(v & m for v in b):
                    found.append((1, f'bit    {name} bit {bit} (0x{m:x}) clear when written, set otherwise'))
            if max(a + b) < (1 << 32) and unique_ratio < 0.9:
                if max(a) < min(b):
                    found.append((2, f'thresh {name}: written <= {hex(max(a))} < never >= {hex(min(b))}'))
                elif max(b) < min(a):
                    found.append((2, f'thresh {name}: never <= {hex(max(b))} < written >= {hex(min(a))}'))
        if not found:
            print('  no single column separates the classes\n')
            continue
        for _, text in sorted(found)[:25]:
            print('  ' + text)
        if len(found) > 25:
            print(f'  ... {len(found) - 25} more')
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
