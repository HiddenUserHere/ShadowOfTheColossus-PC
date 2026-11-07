#!/usr/bin/env python3
"""sotc: merges the pipeline records of this machine's sessions into the shipped pipeline manifest
(SOTC_PIPE_MANIFEST, gpu/sotc/pipe_manifest.h):

    gen_pipeline_manifest.py [--user DIR ...] [--extra FILE ...] [--out FILE] [--check]

Inputs: the user records every session appends (<user>/shader_predict/sotc-pipelines.user.tsv; --user
defaults to user/ next to this repository and may be given several times), any --extra manifest file,
and the current output file (so records are only ever added). Output (default
data/pipeline_manifest/sotc-pipelines.tsv): the records deduplicated by id, sorted (stages by program
hash, then pipelines), one header per record layout (the key / runtime info sizes and the shader meta
version of the build that wrote them; the newest variant key among a layout's records is written as its
header's variant). A pipeline whose stage records are missing is dropped.

The records hold hashes and numeric state only (stage specializations, sharp format fields, pipeline key
bytes). The tool refuses a record that is not of that form, so no game data can reach the file. The
lazy-PSO list (sotc_recent_pso.bin) and the shader caches under user/cache hold no draw state a
pipeline can be rebuilt from, so they are not read. --check: read and report, write nothing.
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEX = re.compile(r'^[0-9a-f]*$')
HEX16 = re.compile(r'^[0-9a-f]{16}$')
HINT_ITEM = re.compile(r'^(I(,\d+){8}|[BA](,\d+){11}|S(,\d+){3})$')
MAX_SPEC_HEX = 64 * 1024   # a stage specialization is a few hundred bytes
MAX_KEY_HEX = 8 * 1024


def layout_of(fields):
    """(format, key, rt, meta) of a header line, and its variant key."""
    values = {}
    for item in fields[1:]:
        name, _, value = item.partition('=')
        values[name] = value
    layout = (values.get('format', ''), values.get('key', ''), values.get('rt', ''), values.get('meta', ''))
    return layout, values.get('variant', '')


def valid_hint(text):
    return text == '-' or all(HINT_ITEM.match(item) for item in text.split(';'))


def read(path, layouts, problems):
    """Adds the records of one file to layouts: layout -> {'variants': [...], 'S': {id: line}, 'G'/'C': {...}}."""
    if not path.is_file():
        return 0
    count = 0
    current = None
    with path.open('r', encoding='ascii', errors='replace', newline='') as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'):
                continue
            fields = line.split('\t')
            kind = fields[0]
            if kind == 'H':
                layout, variant = layout_of(fields)
                current = layouts.setdefault(layout, {'variants': [], 'S': {}, 'G': {}, 'C': {}})
                current['variants'].append(variant)
                continue
            if current is None:
                problems.append(f'{path}:{number}: record before any header')
                continue
            ok = False
            if kind == 'S' and len(fields) == 6:
                ok = (HEX16.match(fields[1]) and HEX16.match(fields[2]) and fields[3].isdigit() and
                      HEX.match(fields[4]) and 0 < len(fields[4]) <= MAX_SPEC_HEX and len(fields[4]) % 2 == 0 and
                      valid_hint(fields[5]))
            elif kind == 'G' and len(fields) == 4:
                stages = fields[3].split(',')
                ok = (HEX16.match(fields[1]) and HEX.match(fields[2]) and 0 < len(fields[2]) <= MAX_KEY_HEX and
                      all(re.match(r'^\d+:[0-9a-f]{16}$', s) for s in stages))
            elif kind == 'C' and len(fields) == 3:
                ok = HEX16.match(fields[1]) and HEX16.match(fields[2])
            if not ok:
                problems.append(f'{path}:{number}: not a manifest record, refused')
                continue
            current[kind].setdefault(fields[1], line)
            count += 1
    return count


def stage_ids(kind, line):
    fields = line.split('\t')
    if kind == 'C':
        return [fields[2]]
    return [item.split(':')[1] for item in fields[3].split(',')]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--user', action='append', type=Path, help='a user folder (default: user/)')
    parser.add_argument('--extra', action='append', type=Path, default=[], help='another manifest file to merge')
    parser.add_argument('--out', type=Path, default=REPO / 'data' / 'pipeline_manifest' / 'sotc-pipelines.tsv')
    parser.add_argument('--check', action='store_true', help='report only, write nothing')
    args = parser.parse_args()
    users = args.user or [REPO / 'user']

    layouts, problems = {}, []
    sources = [args.out] + [u / 'shader_predict' / 'sotc-pipelines.user.tsv' for u in users] + args.extra
    for source in sources:
        n = read(source, layouts, problems)
        print(f'{source}: {n} records' if source.is_file() else f'{source}: none')
    for problem in problems[:20]:
        print(problem, file=sys.stderr)
    if len(problems) > 20:
        print(f'... {len(problems) - 20} more refused lines', file=sys.stderr)

    out_lines = ['# sotc: SOTC_PIPE_MANIFEST pipeline records (gpu/sotc/pipe_manifest.h). Hashes and numeric state only.',
                 '# Made by tools/gen_pipeline_manifest.py; do not edit by hand.']
    totals = {'S': 0, 'G': 0, 'C': 0, 'dropped': 0}
    for layout in sorted(layouts):
        group = layouts[layout]
        pipes = {}
        for kind in ('G', 'C'):
            for pid, line in group[kind].items():
                if all(sid in group['S'] for sid in stage_ids(kind, line)):
                    pipes[(kind, pid)] = line
                else:
                    totals['dropped'] += 1
        used = {sid for (kind, _), line in pipes.items() for sid in stage_ids(kind, line)}
        stages = [line for sid, line in group['S'].items() if sid in used]
        if not pipes:
            continue
        variant = group['variants'][-1] if group['variants'] else ''
        fmt, key, rt, meta = layout
        out_lines.append(f'H\tformat={fmt}\tgame=SotC\tkey={key}\trt={rt}\tmeta={meta}\tvariant={variant}')
        stages.sort(key=lambda line: (line.split('\t')[2], line.split('\t')[1]))
        out_lines += stages
        out_lines += [pipes[k] for k in sorted(pipes, key=lambda k: (k[0] == 'C', k[1]))]
        totals['S'] += len(stages)
        totals['G'] += sum(1 for k in pipes if k[0] == 'G')
        totals['C'] += sum(1 for k in pipes if k[0] == 'C')
    print(f"manifest: {totals['G']} graphics + {totals['C']} compute pipelines, {totals['S']} stages, "
          f"{len(layouts)} layouts; {totals['dropped']} pipelines without their stages dropped")
    if args.check:
        return 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('w', encoding='ascii', newline='\n') as f:
        f.write('\n'.join(out_lines) + '\n')
    print(f'written {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
