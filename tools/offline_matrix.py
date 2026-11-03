#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""offline_matrix: runs flag configurations in the deterministic offline mode and compares them.

    python tools/offline_matrix.py matrix.json [--out DIR] [--dry-run]
    python tools/offline_matrix.py --aggregate DIR          (rebuild summary.md / summary.json from DIR)

matrix.json (Python stdlib only; Windows Python is fine):
    {"base": {"SOTC_GAME_DIR": "...", ...},            environment shared by every configuration
     "repeats": 3, "warmup": 1, "timeout_s": 900,
     "configs": {"base": {}, "flag": {"SOME_SWITCH": "1"}},   the FIRST configuration is the reference
     "frames": [600, 1800]}                            optional inclusive frame window

Each run is one `python sotc.py` (cwd = repo root) with base + configuration environment, plus
SOTC_DETERMINISTIC=1, SOTC_FRAME_METRICS=<run>/metrics.csv and SOTC_FRAME_HASH=<run>/hash. SOTC_* and BB_*
variables of the caller are not inherited, so only the matrix's switches apply. The runner also sets, unless the
configuration sets them: SOTC_HEADLESS=2 (a monitor window with the FPS overlay; --hidden gives 1 = no window),
SOTC_DET_EXIT_FRAMES (the matrix's top-level "exit_frames", else the last frame of "frames" plus a margin), and
SOTC_RUN_LABEL ("<config> r<k>/<repeats>", or "<config> warmup"), SOTC_RUN_INDEX ("<i>/<total runs>") and
SOTC_RUN_FLAGS (the configuration's variables, SOTC_ stripped, cut to 120 characters) for the window title.
Rounds are interleaved:
round k runs every configuration once, so a drift of the machine hits all of them alike (as
tools/pcmem_matrix.py does). The first `warmup` rounds are discarded (shader cache warm-up). A run is
valid when it did not time out, wrote metrics rows and, with a lab route set, logged
"SOTC LAB ROUTE done:". Invalid runs are listed in the summary and left out.

Output folder (default user/offline/<timestamp>):
    runs/<config>/r<round>/   metrics.csv, hashes.csv (or hash), last_run.log, stdout.txt
    runs.json                 manifest (one record per run, rewritten after each run)
    matrix.json               copy of the input
    summary.md, summary.json  the aggregation (see docs/sotc/OFFLINE-TESTS.md)

Aggregation: for each configuration, the frames common to all valid runs of all configurations (inside the
window) are aligned by frame number; per frame the median over the repeats of every metrics column; then the
mean and the p99 of that series over the frames. Each delta is taken against the reference configuration, with
a noise estimate: the min-max spread of the same statistic over the repeats (the larger of the two sides).
Images: a frame is nondeterministic when its exact hash differs between repeats of one configuration; a
real change is a frame stable in the configuration whose exact hash differs from the reference's stable hash,
reported with the pHash Hamming distance.
"""
import argparse
import csv
import json
import re
import math
import os
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROUTE_DONE = 'SOTC LAB ROUTE done:'
DEFAULT_TIMEOUT_S = 900.0
DROPPED_PREFIXES = ('SOTC_', 'BB_')
FORCED_KEYS = ('SOTC_DETERMINISTIC', 'SOTC_FRAME_METRICS', 'SOTC_FRAME_HASH')
STATS = ('mean', 'p99')
HASH_FILES = ('hashes.csv', 'hash.csv', 'hash')
LIST_LIMIT = 30  # frames listed by name in the image section
EXIT_MARGIN_FRAMES = 120  # SOTC_DET_EXIT_FRAMES = last frame of the window + this margin
FLAGS_TEXT_LIMIT = 120  # SOTC_RUN_FLAGS is cut to this many characters (the monitor window title)


# ---------------------------------------------------------------- matrix file

def parse_matrix(data):
    """Validates a matrix mapping and returns the normalized spec (raises ValueError)."""
    if not isinstance(data, dict):
        raise ValueError('the matrix file must be a JSON object')
    base = data.get('base', {})
    if not isinstance(base, dict):
        raise ValueError('"base" must be an object of environment variables')
    configs = data.get('configs')
    if not isinstance(configs, dict) or not configs:
        raise ValueError('"configs" must be a non-empty object: {"name": {ENV...}, ...}')
    repeats = data.get('repeats', 3)
    warmup = data.get('warmup', 1)
    if not isinstance(repeats, int) or isinstance(repeats, bool) or repeats < 1:
        raise ValueError('"repeats" must be an integer >= 1')
    if not isinstance(warmup, int) or isinstance(warmup, bool) or warmup < 0:
        raise ValueError('"warmup" must be an integer >= 0')
    timeout_s = data.get('timeout_s', DEFAULT_TIMEOUT_S)
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) or timeout_s <= 0:
        raise ValueError('"timeout_s" must be a positive number of seconds')
    window = data.get('frames')
    if window is not None:
        if (not isinstance(window, list) or len(window) != 2
                or not all(isinstance(v, int) and not isinstance(v, bool) for v in window)
                or window[0] > window[1]):
            raise ValueError('"frames" must be [from, to] with integers from <= to')
        window = (window[0], window[1])
    exit_frames = data.get('exit_frames')
    if exit_frames is not None and (not isinstance(exit_frames, int) or isinstance(exit_frames, bool)
                                    or exit_frames < 1):
        raise ValueError('"exit_frames" must be an integer >= 1')

    def env_of(name, mapping):
        if not isinstance(mapping, dict):
            raise ValueError(f'{name} must be an object of environment variables')
        for key in mapping:
            if key in FORCED_KEYS:
                raise ValueError(f'{name}: {key} is set by the runner for every run; remove it')
        return {str(k): str(v) for k, v in mapping.items()}

    spec = {
        'base': env_of('"base"', base),
        'configs': {str(name): env_of(f'configuration "{name}"', env) for name, env in configs.items()},
        'repeats': repeats,
        'warmup': warmup,
        'timeout_s': float(timeout_s),
        'frames': window,
        'exit_frames': exit_frames,
    }
    return spec


def exit_frames_for(spec):
    """SOTC_DET_EXIT_FRAMES for every run: the top-level "exit_frames" if given, else the last frame of
    the window plus a margin, else None (the game then runs as its route says)."""
    if spec.get('exit_frames') is not None:
        return spec['exit_frames']
    if spec.get('frames'):
        return spec['frames'][1] + EXIT_MARGIN_FRAMES
    return None


def run_labels(spec, name, rnd, index, total):
    """The SOTC_RUN_* variables that name a run in the monitor window. Measured repeats are numbered from 1
    ("r1/3"); warmup rounds are "warmup". The flags are the configuration's own variables, SOTC_ stripped."""
    if rnd < spec['warmup']:
        label = f'{name} warmup'
    else:
        label = f'{name} r{rnd - spec["warmup"] + 1}/{spec["repeats"]}'
    flags = ' '.join(f'{key[len("SOTC_"):] if key.startswith("SOTC_") else key}={value}'
                     for key, value in spec['configs'][name].items())
    return {'SOTC_RUN_LABEL': label, 'SOTC_RUN_INDEX': f'{index}/{total}',
            'SOTC_RUN_FLAGS': flags[:FLAGS_TEXT_LIMIT]}


def load_matrix(path):
    return parse_matrix(json.loads(Path(path).read_text(encoding='utf-8')))


# ---------------------------------------------------------------- run files

def read_metrics(path):
    """metrics.csv -> {frame: {column: value}}. Rows without a frame number are skipped; empty or
    non-numeric cells are left out of that row."""
    rows = {}
    with open(path, newline='', encoding='utf-8') as handle:
        for raw in csv.DictReader(handle):
            row = {(k or '').strip(): (v or '').strip() for k, v in raw.items() if k is not None}
            try:
                frame = int(float(row.pop('frame')))
            except (KeyError, ValueError):
                continue
            values = {}
            for key, text in row.items():
                try:
                    values[key] = float(text)
                except ValueError:
                    pass
            rows[frame] = values
    return rows


def read_hashes(path):
    """hashes.csv (frame,exact,phash) -> {frame: (exact, phash)}."""
    out = {}
    with open(path, newline='', encoding='utf-8') as handle:
        for raw in csv.DictReader(handle):
            try:
                frame = int(float((raw.get('frame') or '').strip()))
            except ValueError:
                continue
            out[frame] = ((raw.get('exact') or '').strip(), (raw.get('phash') or '').strip())
    return out


ROUTE_START = re.compile(r'SOTC LAB ROUTE start at flip (\d+)')


def route_start_flip(run_dir):
    """The flip the Lab route started on, from the run's log or stdout; None when absent (old builds)."""
    for name in ('last_run.log', 'stdout.txt'):
        path = run_dir / name
        if path.is_file():
            match = ROUTE_START.search(path.read_text(encoding='utf-8', errors='replace'))
            if match:
                return int(match.group(1))
    return None


def find_hashes(run_dir):
    for name in HASH_FILES:
        candidate = run_dir / name
        if candidate.is_file():
            return candidate
    # SOTC_FRAME_HASH names a folder: the port writes <folder>/hashes.csv.
    candidate = run_dir / 'hash' / 'hashes.csv'
    return candidate if candidate.is_file() else None


def phash_distance(a, b):
    """Hamming distance of two hexadecimal pHash strings; None when either is not one."""
    try:
        x, y = int(a, 16), int(b, 16)
    except (TypeError, ValueError):
        return None
    if len(a) != len(b):
        return None
    return bin(x ^ y).count('1')


# ---------------------------------------------------------------- statistics

def percentile99(values):
    """Nearest-rank 99th percentile (with few frames it is the maximum)."""
    ordered = sorted(values)
    return ordered[max(1, math.ceil(0.99 * len(ordered))) - 1]


STAT_FUNCS = {'mean': statistics.fmean, 'p99': percentile99}


def window_frames(metric_maps, window):
    common = set.intersection(*(set(m) for m in metric_maps))
    if window:
        common = {f for f in common if window[0] <= f <= window[1]}
    return sorted(common)


def summarize_timing(runs, frames):
    """{column: {stat: {'value', 'spread'}}}. spread is the min-max over the repeats (None with one)."""
    columns = list(dict.fromkeys(c for r in runs for c in r['metrics'][frames[0]]))
    timing = {}
    for col in columns:
        if any(col not in r['metrics'][f] for r in runs for f in frames):
            continue
        per_run = [[r['metrics'][f][col] for f in frames] for r in runs]
        median_series = [statistics.median(values) for values in zip(*per_run)]
        entry = {}
        for stat, func in STAT_FUNCS.items():
            per = [func(series) for series in per_run]
            entry[stat] = {'value': func(median_series),
                           'spread': max(per) - min(per) if len(per) > 1 else None}
        timing[col] = entry
    return timing


def compare_timing(base, other):
    rows = []
    for col, stats in base.items():
        if col not in other:
            continue
        for stat in STATS:
            b, o = stats[stat], other[col][stat]
            delta = o['value'] - b['value']
            spreads = [s for s in (b['spread'], o['spread']) if s is not None]
            noise = max(spreads) if len(spreads) == 2 else None
            rows.append({
                'metric': col, 'stat': stat, 'base': b['value'], 'value': o['value'], 'delta': delta,
                'delta_pct': delta / b['value'] * 100 if b['value'] else None,
                'noise': noise,
                'above_noise': None if noise is None else abs(delta) > noise,
            })
    return rows


def summarize_images(runs, frames):
    """Stability of the exact hashes over the repeats of one configuration, for the aligned frames."""
    with_hashes = [r for r in runs if r['hashes']]
    if not with_hashes:
        return None
    usable = [f for f in frames
              if all(f in r['hashes'] and r['hashes'][f][0] for r in with_hashes)]
    stable, phash, nondeterministic = {}, {}, []
    for f in usable:
        exacts = [r['hashes'][f][0] for r in with_hashes]
        if len(set(exacts)) == 1:
            stable[f] = exacts[0]
        else:
            nondeterministic.append(f)
        phash[f] = with_hashes[0]['hashes'][f][1]
    return {'runs_with_hashes': len(with_hashes), 'runs': len(runs), 'frames': usable,
            'stable': stable, 'phash': phash, 'nondeterministic': nondeterministic}


def compare_images(base, other):
    """Frames whose stable exact hash differs from the reference's stable hash are real changes."""
    same, changed, unstable = 0, [], []
    for f in sorted(set(base['frames']) & set(other['frames'])):
        b, o = base['stable'].get(f), other['stable'].get(f)
        if b is None or o is None:
            unstable.append(f)
        elif b == o:
            same += 1
        else:
            changed.append({'frame': f, 'phash_distance': phash_distance(base['phash'][f], other['phash'][f])})
    distances = [c['phash_distance'] for c in changed if c['phash_distance'] is not None]
    return {'compared': same + len(changed) + len(unstable), 'same': same, 'changed': changed,
            'unstable': unstable, 'max_phash_distance': max(distances) if distances else None}


def aggregate(by_config, spec):
    """by_config: {config: [loaded run, ...]} with valid, non-warmup runs only (each run has
    'metrics' {frame: {col: value}} and 'hashes' {frame: (exact, phash)}). Returns the result dict."""
    names = list(spec['configs'])
    missing = [n for n in names if not by_config.get(n)]
    if missing:
        raise ValueError(f'no valid run for: {", ".join(missing)}')
    base = names[0]
    every_run = [r for n in names for r in by_config[n]]
    frames = window_frames([r['metrics'] for r in every_run], spec.get('frames'))
    if not frames:
        raise ValueError('no frame is common to all valid runs (check the frame window)')
    timing = {n: summarize_timing(by_config[n], frames) for n in names}
    images = {n: summarize_images(by_config[n], frames) for n in names}
    result = {
        'base': base,
        'frames': {'count': len(frames), 'first': frames[0], 'last': frames[-1],
                   'window': list(spec['frames']) if spec.get('frames') else None},
        'repeats': {n: len(by_config[n]) for n in names},
        'warmup': spec['warmup'],
        'timing': timing,
        'deltas': {n: compare_timing(timing[base], timing[n]) for n in names[1:]},
        'images': images,
        'image_vs_base': {},
    }
    if images[base]:
        for n in names[1:]:
            if images[n]:
                result['image_vs_base'][n] = compare_images(images[base], images[n])
    return result


# ---------------------------------------------------------------- markdown

def fmt(value):
    if value is None:
        return '-'
    if abs(value) >= 100:
        return f'{value:.1f}'
    if abs(value) >= 1:
        return f'{value:.2f}'
    return f'{value:.3f}'


def render_markdown(result, invalid=()):
    base, names = result['base'], list(result['repeats'])
    frames = result['frames']
    lines = [
        '# Offline matrix summary', '',
        f'- Reference configuration: `{base}`',
        f'- Frames: {frames["first"]} to {frames["last"]} ({frames["count"]} aligned on every valid run)',
        '- Repeats (valid runs): ' + ', '.join(f'`{n}` {result["repeats"][n]}' for n in names)
        + f'; warmup rounds discarded: {result["warmup"]}',
        f'- Invalid runs left out: {len(invalid)}', '',
        '## Timing', '',
        'Median over the repeats of each frame, then mean and p99 over the frames. `±` is the min-max spread of '
        'that statistic over the repeats of the configuration (blank with one repeat).', '',
    ]
    columns = list(dict.fromkeys(c for n in names for c in result['timing'][n]))
    lines.append('| metric | stat | ' + ' | '.join(f'`{n}`' + (' (ref)' if n == base else '') for n in names) + ' |')
    lines.append('|' + '---|' * (len(names) + 2))
    for col in columns:
        for stat in STATS:
            cells = []
            for n in names:
                entry = result['timing'][n].get(col, {}).get(stat)
                if entry is None:
                    cells.append('-')
                    continue
                text = fmt(entry['value'])
                if entry['spread'] is not None:
                    text += f' ±{fmt(entry["spread"])}'
                cells.append(text)
            lines.append(f'| {col} | {stat} | ' + ' | '.join(cells) + ' |')
    if result['deltas']:
        lines += ['', '## Difference from the reference', '',
                  'Cell: Δ (Δ %) and ±noise. `*` marks a Δ larger than the noise (the larger repeat spread of the two '
                  'sides); without a noise estimate (one repeat) nothing is starred.', '']
        lines.append(f'| metric | stat | ' + ' | '.join(f'`{n}` vs `{base}`' for n in names[1:]) + ' |')
        lines.append('|' + '---|' * (len(names) + 1))
        for col in columns:
            for stat in STATS:
                cells = []
                for n in names[1:]:
                    row = next((d for d in result['deltas'][n] if d['metric'] == col and d['stat'] == stat), None)
                    if row is None:
                        cells.append('-')
                        continue
                    pct = '' if row['delta_pct'] is None else f' ({row["delta_pct"]:+.1f}%)'
                    text = f'{row["delta"]:+.3f}{pct}'
                    if row['noise'] is not None:
                        text += f' ±{fmt(row["noise"])}'
                    if row['above_noise']:
                        text += ' *'
                    cells.append(text)
                lines.append(f'| {col} | {stat} | ' + ' | '.join(cells) + ' |')
    lines += ['', '## Images', '',
              'Exact hashes: a frame is *nondeterministic* when its hash differs between the repeats of one '
              'configuration (noise, not a visual change). A *changed* frame is stable inside the configuration '
              'and differs from the reference (pHash Hamming distance in brackets). *Unstable* frames cannot be '
              'compared.', '',
              '| configuration | frames | nondeterministic | changed vs ref | unstable vs ref | max pHash distance |',
              '|---|---|---|---|---|---|']
    details = []
    for n in names:
        img = result['images'][n]
        if img is None:
            lines.append(f'| `{n}` | - | no hash file in its runs | - | - | - |')
            continue
        vs = result['image_vs_base'].get(n)
        if n == base:
            changed, unstable, maxd = '(reference)', '-', '-'
        elif vs is None:
            changed, unstable, maxd = '-', '-', '-'
        else:
            changed, unstable = str(len(vs['changed'])), str(len(vs['unstable']))
            maxd = '-' if vs['max_phash_distance'] is None else str(vs['max_phash_distance'])
        lines.append(f'| `{n}` | {len(img["frames"])} | {len(img["nondeterministic"])} | {changed} | '
                     f'{unstable} | {maxd} |')
        if img['nondeterministic']:
            listed = ', '.join(map(str, img['nondeterministic'][:LIST_LIMIT]))
            details.append(f'- `{n}` nondeterministic frames: {listed}'
                           + (' ...' if len(img['nondeterministic']) > LIST_LIMIT else ''))
        if vs and vs['changed']:
            listed = ', '.join(f'{c["frame"]} ({c["phash_distance"]})' for c in vs['changed'][:LIST_LIMIT])
            details.append(f'- `{n}` changed frames vs `{base}`: {listed}'
                           + (' ...' if len(vs['changed']) > LIST_LIMIT else ''))
    if details:
        lines += [''] + details
    if invalid:
        lines += ['', '## Invalid runs (left out)', '', '| configuration | round | reason | exit | timed out |',
                  '|---|---|---|---|---|']
        for rec in invalid:
            lines.append(f'| `{rec["config"]}` | {rec["round"] + 1} | {rec["reason"]} | {rec["exit_code"]} | '
                         f'{"yes" if rec["timed_out"] else "no"} |')
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------- running

def runner_defaults(merged, exit_frames=None, hidden=False):
    """Variables the runner sets for the window and the exit, unless the configuration sets them itself.
    The default window is the monitor window (SOTC_HEADLESS=2, with the FPS overlay); --hidden gives 1."""
    defaults = {'SOTC_HEADLESS': '1' if hidden else '2'}
    if exit_frames is not None:
        defaults['SOTC_DET_EXIT_FRAMES'] = str(exit_frames)
    return {key: value for key, value in defaults.items() if key not in merged}


def run_environment(merged, run_dir, labels=None, exit_frames=None, hidden=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith(DROPPED_PREFIXES)}
    env.update(merged)
    env['SOTC_DETERMINISTIC'] = '1'
    env['SOTC_FRAME_METRICS'] = str(run_dir / 'metrics.csv')
    env['SOTC_FRAME_HASH'] = str(run_dir / 'hash')
    env.update(runner_defaults(merged, exit_frames, hidden))
    env.update(labels or {})
    return env


def kill_tree(process):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        process.kill()


def check_run(run_dir, merged, timed_out):
    """None when the run is valid, else the reason it is not."""
    if timed_out:
        return 'timeout'
    metrics = run_dir / 'metrics.csv'
    if not metrics.is_file():
        return 'no metrics.csv'
    try:
        rows = read_metrics(metrics)
    except (OSError, csv.Error, UnicodeDecodeError) as error:
        return f'metrics.csv unreadable: {error}'
    if not rows:
        return 'metrics.csv has no frame rows'
    if merged.get('SOTC_LAB_ROUTE'):
        log = run_dir / 'last_run.log'
        if not log.is_file() or ROUTE_DONE not in log.read_text(encoding='utf-8', errors='replace'):
            return f'route did not run ("{ROUTE_DONE}" missing from the log)'
    return None


def run_one(merged, run_dir, timeout_s, labels=None, exit_frames=None, hidden=False):
    run_dir.mkdir(parents=True, exist_ok=True)
    for name in ('metrics.csv', *HASH_FILES):  # no stale output from an earlier matrix in this folder
        (run_dir / name).unlink(missing_ok=True)
    env = run_environment(merged, run_dir, labels, exit_frames, hidden)
    started = time.time()
    timed_out = False
    with open(run_dir / 'stdout.txt', 'wb') as sink:
        process = subprocess.Popen([sys.executable, str(ROOT / 'sotc.py')], cwd=str(ROOT), env=env,
                                   stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            timed_out = True
            kill_tree(process)
            code = process.wait()
    # sotc.py writes the session log to <user>/last_run.log; copy it before the next run replaces it.
    user = (ROOT / merged.get('SOTC_USER_DIR', 'user')).resolve()
    log = user / 'last_run.log'
    if log.is_file() and log.stat().st_mtime >= started - 1:
        shutil.copyfile(log, run_dir / 'last_run.log')
    reason = check_run(run_dir, merged, timed_out)
    return {'valid': reason is None, 'reason': reason or '', 'exit_code': code, 'timed_out': timed_out,
            'seconds': round(time.time() - started, 1)}


def write_manifest(out, manifest):
    (out / 'runs.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def execute(spec, out, dry_run=False, hidden=False):
    names = list(spec['configs'])
    total = spec['warmup'] + spec['repeats']
    total_runs = total * len(names)
    exit_frames = exit_frames_for(spec)
    manifest = []
    index = 0
    for rnd in range(total):
        for name in names:
            index += 1
            warm = rnd < spec['warmup']
            merged = {**spec['base'], **spec['configs'][name]}
            labels = run_labels(spec, name, rnd, index, total_runs)
            rel = Path('runs') / name / f'r{rnd}'
            if dry_run:
                print(f'[round {rnd + 1}/{total}] {name}{" (warmup)" if warm else ""} {rel.as_posix()}')
                for key in sorted(merged):
                    print(f'    {key}={merged[key]}')
                for key, value in {**runner_defaults(merged, exit_frames, hidden), **labels}.items():
                    print(f'    {key}={value}  (runner)')
                continue
            print(f'[round {rnd + 1}/{total}] {name}{" (warmup)" if warm else ""} -> {rel.as_posix()}',
                  flush=True)
            record = {'config': name, 'round': rnd, 'warmup': warm, 'dir': rel.as_posix()}
            record.update(run_one(merged, out / rel, spec['timeout_s'], labels, exit_frames, hidden))
            print(f'    {"ok" if record["valid"] else "INVALID: " + record["reason"]} '
                  f'({record["seconds"]} s, exit {record["exit_code"]})', flush=True)
            manifest.append(record)
            write_manifest(out, manifest)
    return manifest


def load_runs(manifest, out):
    """Valid non-warmup runs grouped by configuration, plus the invalid records."""
    by_config, invalid = {}, []
    for rec in manifest:
        if rec['warmup']:
            continue
        if not rec['valid']:
            invalid.append(rec)
            continue
        run_dir = out / rec['dir']
        hash_file = find_hashes(run_dir)
        # Frames are counted from the route start ("SOTC LAB ROUTE start at flip N"), not from the boot: the load
        # before gameplay takes a different number of frames in every run.
        start = route_start_flip(run_dir)
        metrics = read_metrics(run_dir / 'metrics.csv')
        hashes = read_hashes(hash_file) if hash_file else {}
        if start is not None:
            metrics = {f - start: v for f, v in metrics.items() if f >= start}
            hashes = {f - start: v for f, v in hashes.items() if f >= start}
        by_config.setdefault(rec['config'], []).append({
            'config': rec['config'], 'round': rec['round'], 'start_flip': start,
            'metrics': metrics, 'hashes': hashes,
        })
    return by_config, invalid


def write_summary(out, result, invalid):
    (out / 'summary.md').write_text(render_markdown(result, invalid), encoding='utf-8')
    (out / 'summary.json').write_text(json.dumps({'result': result, 'invalid_runs': invalid}, indent=2,
                                                 default=str) + '\n', encoding='utf-8')


def main(argv=None):
    parser = argparse.ArgumentParser(description='Runs flag configurations in the deterministic offline mode '
                                                 'and compares them (docs/sotc/OFFLINE-TESTS.md).')
    parser.add_argument('matrix', nargs='?', help='matrix JSON file')
    parser.add_argument('--out', help='output folder (default user/offline/<timestamp>)')
    parser.add_argument('--aggregate', metavar='DIR', help='rebuild summary.md / summary.json from DIR (no runs)')
    parser.add_argument('--dry-run', action='store_true', help='print the planned runs and their environment')
    parser.add_argument('--hidden', action='store_true',
                        help='runs without a window (SOTC_HEADLESS=1) instead of the monitor window (=2)')
    args = parser.parse_args(argv)
    try:
        if args.aggregate:
            out = Path(args.aggregate).resolve()
            spec = load_matrix(out / 'matrix.json')
            manifest = json.loads((out / 'runs.json').read_text(encoding='utf-8'))
        else:
            if not args.matrix:
                parser.error('give a matrix JSON file (or --aggregate DIR)')
            spec = load_matrix(args.matrix)
            out = Path(args.out).resolve() if args.out else (
                ROOT / 'user' / 'offline' / datetime.now().strftime('%Y%m%d-%H%M%S')).resolve()
            if args.dry_run:
                execute(spec, out, dry_run=True, hidden=args.hidden)
                return 0
            out.mkdir(parents=True, exist_ok=True)
            (out / 'matrix.json').write_text(Path(args.matrix).read_text(encoding='utf-8'), encoding='utf-8')
            manifest = execute(spec, out, hidden=args.hidden)
        by_config, invalid = load_runs(manifest, out)
        result = aggregate(by_config, spec)
    except (OSError, ValueError, KeyError, json.JSONDecodeError, csv.Error) as error:
        print(f'offline_matrix: {error}', file=sys.stderr)
        return 1
    write_summary(out, result, invalid)
    print(f'summary: {out / "summary.md"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
