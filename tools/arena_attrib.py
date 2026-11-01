#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ranked table of the guest writers of the per-frame arena (SOTC_ARENA_ATTRIB=1, gpu/sotc/arena_attrib.cpp).

Reads the "SOTC ARENA ATTRIB" lines of a session log and names each eboot offset with the function
that contains it (tools/re/eboot_re.py index, out/re/index.pkl.gz; build it once with
`python tools/re/eboot_re.py build`).

Source of the counts: the FINAL lines (written at exit, cumulative) when the log has them. Without
them (the process was killed), the window lines are summed per offset.

usage:
  python tools/arena_attrib.py [LOG] [--kind rewrite|first|both] [--top N] [--no-names]

LOG defaults to user/last_run.log (the session log, see CLAUDE.md).
"""
import argparse
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 're'))

# "SOTC ARENA ATTRIB [FINAL ]REWRITE|FIRST <rank> off=<offset|host:addr> n=<count> thread=<name/tid>
#  caller=<..> caller2=<..> frames=<a>-<b>"
LINE = re.compile(
    r'SOTC ARENA ATTRIB (?P<final>FINAL )?(?P<kind>REWRITE|FIRST) +(?P<rank>\d+) '
    r'off=(?P<off>\S+) n=(?P<n>\d+) thread=(?P<thread>\S+) caller=(?P<caller>\S+) '
    r'caller2=(?P<caller2>\S+) frames=(?P<f0>\d+)-(?P<f1>\d+)')


def parse(path):
    """Returns (final, windows): final rows and window rows, each a list of dicts."""
    final, windows = [], []
    with open(path, encoding='utf-8', errors='replace') as f:
        for text in f:
            m = LINE.search(text)
            if not m:
                continue
            row = dict(kind=m['kind'].lower(), off=m['off'], n=int(m['n']), thread=m['thread'],
                       caller=m['caller'], caller2=m['caller2'], f0=int(m['f0']), f1=int(m['f1']))
            (final if m['final'] else windows).append(row)
    return final, windows


def merge(final, windows):
    """One row per (kind, off): the final counts, or the window counts summed."""
    if final:
        return {(r['kind'], r['off']): r for r in final}
    merged = {}
    for r in windows:
        key = (r['kind'], r['off'])
        if key not in merged:
            merged[key] = dict(r)
        else:
            m = merged[key]
            m['n'] += r['n']
            m['f0'] = min(m['f0'], r['f0'])
            m['f1'] = max(m['f1'], r['f1'])
            m['thread'], m['caller'], m['caller2'] = r['thread'], r['caller'], r['caller2']
    return merged


def name_of(index, text):
    """'sub_cefa0+0x1c' style name for an offset ('0x..'), or the text unchanged (host addresses)."""
    if index is None or not text.startswith('0x'):
        return text
    return index.name(int(text, 16))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', nargs='?', default=str(ROOT / 'user' / 'last_run.log'))
    ap.add_argument('--kind', choices=('rewrite', 'first', 'both'), default='both')
    ap.add_argument('--top', type=int, default=0, help='rows per kind (0: 40 rewrite, 20 first)')
    ap.add_argument('--no-names', action='store_true', help='skip the function names (no index needed)')
    a = ap.parse_args()

    final, windows = parse(a.log)
    if not final and not windows:
        raise SystemExit(f'no "SOTC ARENA ATTRIB" lines in {a.log} (run with SOTC_ARENA_ATTRIB=1)')
    rows = merge(final, windows)

    index = None
    if not a.no_names:
        try:
            import eboot_re
            index = eboot_re.Index()
        except SystemExit as e:
            print(f'(no function names: {e})', file=sys.stderr)

    source = 'FINAL lines' if final else f'{len(windows)} window lines summed'
    print(f'source: {source} from {a.log}')
    kinds = ('rewrite', 'first') if a.kind == 'both' else (a.kind,)
    for kind in kinds:
        top = a.top or (40 if kind == 'rewrite' else 20)
        picked = sorted((r for (k, _), r in rows.items() if k == kind), key=lambda r: -r['n'])[:top]
        total = sum(r['n'] for (k, _), r in rows.items() if k == kind)
        print()
        print(f'{kind.upper()} faults: {total} in the listed rips (top {len(picked)})')
        print(f'{"#":>3} {"count":>9} {"offset":>10}  {"function":<40} {"thread":<36} {"caller":<10} '
              f'{"caller function":<40} {"frames":>13}')
        for i, r in enumerate(picked, 1):
            print(f'{i:>3} {r["n"]:>9} {r["off"]:>10}  {name_of(index, r["off"])[:40]:<40} '
                  f'{r["thread"][:36]:<36} {r["caller"]:<10} '
                  f'{name_of(index, r["caller"])[:40]:<40} {r["f0"]:>6}-{r["f1"]:<6}')
    # Per-caller roll-up: the faults of one caller function, whatever RIP wrote.
    if index is not None and kinds:
        print()
        print('by caller function (rewrite + first, top 15):')
        by_caller = collections.Counter()
        for (k, _), r in rows.items():
            if r['caller'] != '-':
                by_caller[name_of(index, r['caller'])] += r['n']
        for name, n in by_caller.most_common(15):
            print(f'  {n:>9}  {name}')


if __name__ == '__main__':
    main()
