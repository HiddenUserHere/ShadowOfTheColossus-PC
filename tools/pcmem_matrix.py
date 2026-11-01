#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""pcmem_matrix: runs the PC memory model switches over the Lab test route and tabulates the logs.

    python tools/pcmem_matrix.py --dry-run [--configs ...] [--env KEY=VALUE] [--class-pools NAMES]
    python tools/pcmem_matrix.py [--configs base,verify,safe] [--repeat 2] [--out DIR]
                                 [--env KEY=VALUE ...] [--class-pools NAMES]
    python tools/pcmem_matrix.py --parse-only LOG [LOG ...]

Each run is one launch of `python sotc.py` (cwd = repo root) with the common Lab route environment
(SOTC_LAB_ROUTE=user/routes/tests.json, SOTC_CVARS="skipSplash=1 nointro" plus the culling cvars, SOTC_FPS=1) and the
configuration's switches. The game plays the route and closes itself. A run is valid only when its log
has "SOTC LAB ROUTE done:"; a run without it is reported as "route did not run" (with --retry it is
retried once first). One round (--repeat 1) and no retry are the defaults: N configurations = N runs. A run that is still going after RUN_TIMEOUT_S seconds is killed with its
process tree. Inherited SOTC_* variables are removed, so only the configuration's switches apply.

--env KEY=VALUE (repeatable) adds or overrides a variable in every configuration (applied last).
--class-pools NAMES sets SOTC_CLASS_POOLS in the configurations that have that key (heaps); empty means
the game default (PageAllocator).

Configurations are interleaved in rounds: round 1 runs every configuration once, round 2 again, so a
drift in the machine hits all of them alike. Output (default user/pcmem/<timestamp>/):
    logs/<config>-r<rep>-a<attempt>.log   one log per attempt
    summary.md                            rewritten after every run

The first 5 s window (boot) is left out of the averages. Crash markers: lines with "exception",
"Unhandled" or "bad PM4", a log without "SOTC LAB ROUTE exit", and "0x49500" on lines that are not
readback-offset reports (those lines, "eboot+0x49500 N readbacks/s", are in every healthy run).
Warnings (a column, not a crash): "SOTC CLASS TRIP" and "disabled native" lines.

Parsed report lines (the last line of each kind per run; the pool block is the last complete
"SOTC PC POOL:" report): ring full / coverage / hot mask / apply, CLASS heap, COMMIT, RB class, RB CLASS,
STATIC CLASS, RT CLASS, PC POOL. Ring divergence is read from the "verify divergent N of M pages" form
(older builds) or the "divergent N bytes in M binds" form (current builds).
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN_TIMEOUT_S = 600.0        # a run still going after this is killed (its process tree)
RUN_ATTEMPTS = 1             # one attempt per run; --retry retries a run whose route did not start
BOOT_WINDOW_S = 5.0          # the 5 s window (t = 5) is boot: left out of the averages
ROUTE_DONE = 'SOTC LAB ROUTE done:'
ROUTE_EXIT = 'SOTC LAB ROUTE exit'

COMMON_ENV = {
    'SOTC_GAME_DIR': 'C:/Users/ptr/Documents/SOTC/CUSA08809',
    'SOTC_SYS_MODULES': 'C:/Users/ptr/Documents/SOTC/user/sys_modules',
    'SOTC_LAB': '1',
    'SOTC_LAB_ROUTE': 'user/routes/tests.json',
    # sotc: the users' culling settings (sotc.py and the launcher default): without them every Lab run had
    # the GPU index-buffer culling on (~2100 extra dispatches a frame, 1 Nov).
    'SOTC_CVARS': 'skipSplash=1 nointro doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0',
    'SOTC_FPS': '1',
    # sotc: the "SOTC DRAW WORK" column needs the draw thread timers (SOTC_DRAW_SPLIT, sotc_stats.h); they add
    # a clock read per op, so the draw busy numbers of these runs are not comparable with runs from before 1 Nov 2026.
    'SOTC_DRAW_SPLIT': '1',
    'SOTC_DRAW_WORK': '1',
}


def merge_switches(name, *parts):
    """Union of several switch dicts; two different values for one variable is an error."""
    out = {}
    for part in parts:
        for key, value in part.items():
            if out.get(key, value) != value:
                raise SystemExit(f'configuration {name}: switch {key} has two values')
            out[key] = value
    return out


SWITCHES = {
    # The pre-ring8 reference: sotc.py turns SOTC_PC_RING=8 on by default since 1 Nov, so every other
    # configuration without its own SOTC_PC_RING runs on top of ring8.
    'base': {'SOTC_RAM_REPORT': '1', 'SOTC_PC_RING': '0'},
    # The current defaults (sotc.py: ring8 and its hooks), nothing else set.
    'default': {'SOTC_RAM_REPORT': '1'},
    # drawphases bisect (1 Nov): one group each, on top of the defaults.
    'dp_gen': {'SOTC_GEN_BIND': '1', 'SOTC_GEN_DMA': '1', 'SOTC_GEN_TEX': '1'},
    'dp_pr': {'SOTC_PARALLEL_RECORD': '1', 'SOTC_GEN_BIND': '1', 'SOTC_PR_GEN': '1'},
    'dp_pipe': {'SOTC_PIPE_DIRTY': '1', 'SOTC_PIPE_GPL': '1'},
    'dp_img': {'SOTC_IMG_POOLED': '1', 'SOTC_IMG_CREATE_AHEAD': '1', 'SOTC_RT_MAXEXTENT': '1'},
    'dp_class': {'SOTC_STATIC_CLASS': '3', 'SOTC_RB_CLASS': '1', 'SOTC_RB_OWNER': '1',
                 'SOTC_RB_MATERIALIZE': '1', 'SOTC_PARTICLE_PREV': '1', 'SOTC_FLIP_OFFTHREAD': '1'},
    # The groups that cost nothing in the bisect (1 Nov), together; no resolve workers (dp_pr cost ~3 ms).
    'drawbest': {'SOTC_GEN_BIND': '1', 'SOTC_GEN_DMA': '1', 'SOTC_GEN_TEX': '1', 'SOTC_PIPE_DIRTY': '1',
                 'SOTC_PIPE_GPL': '1', 'SOTC_IMG_POOLED': '1', 'SOTC_IMG_CREATE_AHEAD': '1', 'SOTC_RT_MAXEXTENT': '1',
                 'SOTC_STATIC_CLASS': '3', 'SOTC_RB_CLASS': '1', 'SOTC_RB_OWNER': '1',
                 'SOTC_RB_MATERIALIZE': '1', 'SOTC_PARTICLE_PREV': '1', 'SOTC_FLIP_OFFTHREAD': '1'},
    # The draw-thread phases that ran clean on 1 Nov, on top of the defaults (no honest labels and what
    # needs them: SOTC_FRAME_AHEAD, SOTC_RB_HOST, heap serving; no SOTC_BDA, unsafe with ring8).
    'drawphases': {'SOTC_RAM_REPORT': '1', 'SOTC_FLIP_OFFTHREAD': '1', 'SOTC_GEN_BIND': '1', 'SOTC_GEN_DMA': '1',
                   'SOTC_GEN_TEX': '1', 'SOTC_PARALLEL_RECORD': '1', 'SOTC_PR_GEN': '1', 'SOTC_STATIC_CLASS': '3',
                   'SOTC_RB_CLASS': '1', 'SOTC_RB_OWNER': '1', 'SOTC_RB_MATERIALIZE': '1',
                   'SOTC_PARTICLE_PREV': '1', 'SOTC_IMG_POOLED': '1', 'SOTC_IMG_CREATE_AHEAD': '1',
                   'SOTC_RT_MAXEXTENT': '1', 'SOTC_PIPE_DIRTY': '1', 'SOTC_PIPE_GPL': '1'},
    # Current configurations.
    'verify': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '4',
               'SOTC_PC_POOL': '2', 'SOTC_CLASS_TRACK': '2', 'SOTC_HOOK_MEMTYPE_PROBE': '1',
               'SOTC_MEMTYPE_CLASSES': '1', 'SOTC_RB_OWNER': '1', 'SOTC_RB_MATERIALIZE': '1',
               'SOTC_RB_CLASS': '1', 'SOTC_PARTICLE_PREV': '1', 'SOTC_STATIC_CLASS': '2',
               'SOTC_COMMIT_CHUNKS': '2'},
    'ring5': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '5'},
    'chunks2': {'SOTC_COMMIT_CHUNKS': '2'},
    # The allocator probe alone: several pieces need it, so its own cost is measured apart.
    'probe': {'SOTC_HOOK_ALLOC_PROBE': '1'},
    # Readback isolation (vertex explosions seen in rb2, 1 Nov): the old heuristic, then + class.
    'rbheur': {'SOTC_RB_OWNER': '1', 'SOTC_RB_MATERIALIZE': '1'},
    'rbclass1': {'SOTC_RB_OWNER': '1', 'SOTC_RB_MATERIALIZE': '1', 'SOTC_RB_CLASS': '1',
                 'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_PARTICLE_PREV': '1'},
    'heaps': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_MEMTYPE_CLASSES': '1',
              'SOTC_CLASS_TRACK': '1', 'SOTC_CLASS_POOLS': ''},
    'poolnative': {'SOTC_PC_POOL_NATIVE': '1', 'SOTC_HOOK_ALLOC_PROBE': '1'},
    # Native pool with the flex-node path too (SOTC_PC_POOL=2 must stay unset: one mode per session;
    # the native module verifies its own calls).
    'poolflex': {'SOTC_PC_POOL_NATIVE': '1', 'SOTC_HOOK_ALLOC_PROBE': '1',
                 'SOTC_PC_POOL_PATHS': 'flex-exact,primary-exact,flex-node'},
    # Native pool with every wired path (fresh-reserve and POOL_FIX are not wired yet).
    'poolall': {'SOTC_PC_POOL_NATIVE': '1', 'SOTC_HOOK_ALLOC_PROBE': '1',
                'SOTC_PC_POOL_PATHS': 'flex-exact,primary-exact,flex-node,primary-node,fresh-plain,fresh-spill'},
    # Full ring, protect then copy, with verify (=7).
    'ring7': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '7'},
    # Draw thread: the fast switches together, then the same with their verify / timing modes.
    'drawfast': {'SOTC_VK_POOL': '1',
                 'SOTC_PROTECT_BATCH': '1', 'SOTC_PRESENT_UNLOCKED': '1'},
    'drawverify': {'SOTC_TEX_BLOCK': '2',
                   'SOTC_PRESENT_UNLOCKED': '2', 'SOTC_CMD_STATS': '1'},
    # Arena rewrite attribution (who writes into the per-frame arena after a submit).
    'attrib': {'SOTC_ARENA_ATTRIB': '1', 'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1'},
    # Full ring, GPU reads the arena from imported host memory (no copy, no protection).
    'ring8': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '8'},
    # Native pool, every wired path (incl. the fresh reserve-map and POOL_FIX compositions).
    'poolfull': {'SOTC_PC_POOL_NATIVE': '1', 'SOTC_HOOK_ALLOC_PROBE': '1',
                 'SOTC_PC_POOL_PATHS': 'flex-exact,primary-exact,flex-node,primary-node,fresh-plain,'
                                       'fresh-spill,fresh-reserve,fresh-poolfix'},
    # The proven switches together: ring8 (host arena binds) plus the native pool on every wired path.
    'ring8pool': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '8',
                  'SOTC_PC_POOL_NATIVE': '1',
                  'SOTC_PC_POOL_PATHS': 'flex-exact,primary-exact,flex-node,primary-node,fresh-plain,'
                                        'fresh-spill,fresh-reserve,fresh-poolfix'},
    # CPU_ONLY publish (mode 1) with its probes, and placement observe/verify.
    'cpuonly': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_MEMTYPE_CLASSES': '1',
                'SOTC_CLASS_TRACK': '1', 'SOTC_PC_PLACE': '2'},
    # CPU_ONLY cost split: publish without the placement glue, and the placement glue alone.
    'cpuonly1': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_MEMTYPE_CLASSES': '1',
                 'SOTC_CLASS_TRACK': '1'},
    'place2': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_PC_PLACE': '2'},
    # Readback copied right after the GPU writer (no stale data), on the readback owner set.
    'rbahead': {'SOTC_RB_OWNER': '1', 'SOTC_RB_MATERIALIZE': '1', 'SOTC_RB_CLASS': '1',
                'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_PARTICLE_PREV': '1', 'SOTC_HOOK_XPR_PROBE': '1',
                'SOTC_RB_CLASS_AHEAD': '1'},
    # Pieces written by the 1 Nov agents and not yet run in game: native node-pool refill on top of
    # poolfull, the texture protect batch (rasterizer scope), and the leaked-VA quarantine (counts only).
    'poolrefill': {'SOTC_PC_POOL_NATIVE': '1', 'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_PC_POOL_REFILL': '1',
                   'SOTC_PC_POOL_PATHS': 'flex-exact,primary-exact,flex-node,primary-node,fresh-plain,'
                                         'fresh-spill,fresh-reserve,fresh-poolfix'},
    'texbatch': {'SOTC_TEX_PROTECT_BATCH': '1'},
    'recycle1': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_PC_RECYCLE': '1'},
    # ring8 heap serving: CPUMemory granules the GPU binds are served from host memory like the arena.
    # ring8probe is the control (the same probes and class shadow, no heap serving).
    'ring8probe': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '8',
                   'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_MEMTYPE_CLASSES': '1', 'SOTC_CLASS_TRACK': '2'},
    'ring8heaps': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '8',
                   'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_MEMTYPE_CLASSES': '1', 'SOTC_CLASS_TRACK': '2',
                   'SOTC_PC_RING8_HEAPS': 'CPUMemory'},
    # ring8 heap serving: CPUMemory granules the GPU binds are served from host memory like the arena.
    # ring8probe is the control (the same probes and class shadow, no heap serving).
    'ring8probe': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '8',
                   'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_MEMTYPE_CLASSES': '1', 'SOTC_CLASS_TRACK': '2'},
    'ring8heaps': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_HOOK_FRAME_FLIP': '1', 'SOTC_PC_RING': '8',
                   'SOTC_HOOK_MEMTYPE_PROBE': '1', 'SOTC_MEMTYPE_CLASSES': '1', 'SOTC_CLASS_TRACK': '2',
                   'SOTC_PC_RING8_HEAPS': 'CPUMemory'},
    # Every draw-thread switch, plus the RAM breakdown.
    'drawall': {'SOTC_VK_POOL': '1',
                'SOTC_PROTECT_BATCH': '1', 'SOTC_PRESENT_UNLOCKED': '1',
                'SOTC_RAM_REPORT': '1'},
    # Old configurations, kept under their names.
    'ring': {'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_PC_RING': '1'},
    'static': {'SOTC_STATIC_CLASS': '1'},
    'rb': {'SOTC_RB_OWNER': '1', 'SOTC_RB_MATERIALIZE': '1', 'SOTC_RB_CLASS': '1',
           'SOTC_HOOK_ALLOC_PROBE': '1', 'SOTC_PARTICLE_PREV': '1'},
    'pool2': {'SOTC_PC_POOL': '2', 'SOTC_HOOK_ALLOC_PROBE': '1'},
    'commit': {'SOTC_COMMIT_ON_DEMAND': '1'},
}
# "safe": ring5, chunks2 and rbclass1 with the static class (the probes they need are in the parts).
SWITCHES['safe'] = merge_switches('safe', SWITCHES['ring5'], SWITCHES['chunks2'], SWITCHES['rbclass1'],
                                  {'SOTC_STATIC_CLASS': '1'})
# "all" (old): the union of ring, static, rb and commit (not pool2).
SWITCHES['all'] = merge_switches('all', SWITCHES['ring'], SWITCHES['static'], SWITCHES['rb'],
                                 SWITCHES['commit'])
DEFAULT_CONFIGS = ['base', 'verify', 'ring5', 'chunks2', 'heaps', 'poolnative', 'safe']

ROUTE_RE = re.compile(r'SOTC LAB ROUTE done: (\d+) frames, p50 ([\d.]+) ms, p99 ([\d.]+) ms, '
                      r'max ([\d.]+) ms, hitches >33 ms (\d+), >50 ms (\d+), >100 ms (\d+)')
FPS_RE = re.compile(r'SOTC FPS ([\d.]+) at ([\d.]+)s;')
DRAW_RE = re.compile(r'SOTC threads per frame: draw busy ([\d.]+) ms')
DRAW_WORK_RE = re.compile(r'SOTC DRAW WORK: ([\d.]+) ms/frame')  # sotc: SOTC_DRAW_WORK, printed before the FPS line
DMA_RE = re.compile(r'SOTC DMA sweeps ([\d.]+)/s,.*?; ([\d.]+) ms/s')
FAULTS_RE = re.compile(r'SOTC WRITE FAULTS: eboot ([\d.]+)/s, bpe heap ([\d.]+)/s, host module ([\d.]+)/s')
READBACK_RE = re.compile(r'SOTC READBACK RIPS: ([\d.]+) readbacks/s')  # waited for the GPU

# The last line of each kind is kept per run (key -> pattern matched at the start of the stripped line).
CONFIG_LINES = [
    ('PC RING full', re.compile(r'SOTC PC RING full \(mode ')),
    ('PC RING coverage', re.compile(r'SOTC PC RING coverage \(')),
    ('PC RING hot mask', re.compile(r'SOTC PC RING hot mask \(')),
    ('PC RING apply', re.compile(r'SOTC PC RING apply \(')),
    ('CLASS heap', re.compile(r'SOTC CLASS heap ')),
    ('COMMIT', re.compile(r'SOTC COMMIT \(')),
    ('RB class', re.compile(r'SOTC RB class \(')),
    ('RB CLASS mask', re.compile(r'SOTC RB CLASS: \d+ mask arena blocks')),
    ('RB CLASS stats', re.compile(r'SOTC RB CLASS: stats reads checked')),
    ('STATIC CLASS', re.compile(r'SOTC STATIC CLASS: ')),
    ('RT CLASS', re.compile(r'SOTC RT CLASS: ')),
]
CONFIG_LINE_KEYS = [key for key, _ in CONFIG_LINES] + ['PC POOL']
# The PC POOL report is a block: a line "SOTC PC POOL: allocs ..." starts it, and the lines
# "SOTC PC POOL: ..." and "SOTC PC POOL path ..." after it belong to it. Per-event lines
# (MISMATCH, FREE, WOULD CRASH, unmodelled) are not part of the block.
POOL_BLOCK_START = re.compile(r'SOTC PC POOL: allocs ')
POOL_BLOCK_LINE = re.compile(r'SOTC PC POOL(: | path )')
POOL_ALLOCS_RE = re.compile(r'SOTC PC POOL: allocs (\d+), frees (\d+), matches (\d+), mismatches (\d+), '
                            r'would-crash (\d+)')
POOL_ELIGIBLE_RE = re.compile(r'SOTC PC POOL: eligible calls (\d+), of them mismatched (\d+)')
RING_DIVERGENT_PAGES_RE = re.compile(r'verify divergent (\d+) of (\d+) pages')   # older builds
RING_DIVERGENT_BYTES_RE = re.compile(r'divergent (\d+) bytes in (\d+) binds')    # current builds
NO_EXTENT_RE = re.compile(r'of which ([\d.]+)% in no extent')
COMMIT_MIB_RE = re.compile(r'([\d.]+) MiB of guest memory committed')
WARNING_RE = re.compile(r'SOTC CLASS TRIP|disabled native', re.IGNORECASE)
CRASH_WORDS = ('exception', 'unhandled', 'bad pm4')
CRASH_OFFSET = '0x49500'
SAMPLE_LIMIT = 5


# --- parsing -----------------------------------------------------------------------------------------

def extract_metrics(record):
    """Per-run numbers for the switch table, from the kept lines (None when the line is absent)."""
    lines = record['config_lines']
    metrics = {'div': None, 'div_unit': None, 'no_extent': None, 'pool_matches': None,
               'pool_mismatches': None, 'native_calls': None, 'would_crash': None, 'committed_mib': None}
    ring = lines.get('PC RING full', '')
    match = RING_DIVERGENT_PAGES_RE.search(ring)
    if match:
        metrics['div'], metrics['div_unit'] = float(match.group(1)), 'pages'
    else:
        match = RING_DIVERGENT_BYTES_RE.search(ring)
        if match:
            metrics['div'], metrics['div_unit'] = float(match.group(1)), 'bytes'
    match = NO_EXTENT_RE.search(lines.get('PC RING coverage', ''))
    if match:
        metrics['no_extent'] = float(match.group(1))
    match = COMMIT_MIB_RE.search(lines.get('COMMIT', ''))
    if match:
        metrics['committed_mib'] = float(match.group(1))
    for line in record['pool_block']:
        match = POOL_ALLOCS_RE.search(line)
        if match:
            metrics['pool_matches'] = float(match.group(3))
            metrics['pool_mismatches'] = float(match.group(4))
            metrics['would_crash'] = float(match.group(5))
        match = POOL_ELIGIBLE_RE.search(line)
        if match:
            metrics['native_calls'] = float(match.group(1))
    return metrics


def parse_log(text):
    """One log as a dict: 'route' (None when the route did not finish), 'exit_seen', 'windows' (one
    dict per "SOTC FPS" line: t, fps, draw, dma_ms, readbacks, eboot, bpe, host), 'config_lines'
    (key -> the last line of that kind), 'pool_block' (the last complete PC POOL report),
    'warnings' (count of warning lines), 'warning_samples', 'metrics' and 'crash' (sample marker lines)."""
    record = {'route': None, 'exit_seen': False, 'windows': [], 'config_lines': {}, 'pool_block': [],
              'warnings': 0, 'warning_samples': [], 'crash': []}
    window = None
    pending_draw = None  # "threads per frame" is printed before the FPS line of its window
    pending_draw_work = None  # sotc: "SOTC DRAW WORK" likewise
    pool_block = None    # the PC POOL report being read (None before the first one)
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if ROUTE_EXIT in line:
            record['exit_seen'] = True
        match = ROUTE_RE.search(line)
        if match:
            frames, p50, p99, peak, h33, h50, h100 = match.groups()
            record['route'] = {'frames': int(frames), 'p50': float(p50), 'p99': float(p99),
                               'max': float(peak), 'hitch33': int(h33), 'hitch50': int(h50),
                               'hitch100': int(h100)}
        match = FPS_RE.search(line)
        if match:
            window = {'t': float(match.group(2)), 'fps': float(match.group(1)), 'draw': pending_draw,
                      'draw_work': pending_draw_work, 'dma_ms': None, 'readbacks': None, 'eboot': None,
                      'bpe': None, 'host': None}
            record['windows'].append(window)
            pending_draw = None
            pending_draw_work = None
            continue
        match = DRAW_RE.search(line)
        if match:
            pending_draw = float(match.group(1))
            continue
        match = DRAW_WORK_RE.search(line)
        if match:
            pending_draw_work = float(match.group(1))
            continue
        if window is not None:
            match = DMA_RE.search(line)
            if match:
                window['dma_ms'] = float(match.group(2))
            match = READBACK_RE.search(line)
            if match:
                window['readbacks'] = float(match.group(1))
            match = FAULTS_RE.search(line)
            if match:
                window['eboot'], window['bpe'], window['host'] = (float(g) for g in match.groups())
        for key, pattern in CONFIG_LINES:
            if pattern.match(line):
                record['config_lines'][key] = line
                break
        if POOL_BLOCK_START.match(line):
            pool_block = [line]
        elif pool_block is not None and POOL_BLOCK_LINE.match(line):
            pool_block.append(line)
        if WARNING_RE.search(line):
            record['warnings'] += 1
            if len(record['warning_samples']) < SAMPLE_LIMIT:
                record['warning_samples'].append(line)
        lowered = line.lower()
        if any(word in lowered for word in CRASH_WORDS) or \
                (CRASH_OFFSET in line and 'readbacks/s' not in line):
            if len(record['crash']) < SAMPLE_LIMIT:
                record['crash'].append(line)
    record['pool_block'] = pool_block or []
    if not record['exit_seen']:
        record['crash'].append(f'no "{ROUTE_EXIT}" line (the log ends early)')
    record['metrics'] = extract_metrics(record)
    return record


def mean_or_none(values):
    values = [v for v in values if v is not None]
    return statistics.mean(values) if values else None


def run_stats(record):
    """Averages of one run over its windows after boot."""
    steady = [w for w in record['windows'] if w['t'] > BOOT_WINDOW_S]
    stats = {'windows': len(steady)}
    for key in ('fps', 'draw', 'draw_work', 'dma_ms', 'readbacks', 'eboot', 'bpe', 'host'):
        stats[key] = mean_or_none([w[key] for w in steady])
    return stats


# --- running -----------------------------------------------------------------------------------------

def kill_tree(process):
    if process.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        process.kill()


def launch(switches, log_path):
    """One launch of sotc.py, its output in log_path. Returns (returncode, timed_out, seconds)."""
    env = {k: v for k, v in os.environ.items() if not k.startswith('SOTC_')}
    env.update(COMMON_ENV)
    env.update(switches)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with open(log_path, 'wb') as handle:
        process = subprocess.Popen([sys.executable, str(ROOT / 'sotc.py')], cwd=str(ROOT), env=env,
                                   stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT)
        timed_out = False
        try:
            process.wait(timeout=RUN_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            timed_out = True
            kill_tree(process)
        except BaseException:  # Ctrl+C and the like: do not leave the game running
            kill_tree(process)
            raise
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass
    return process.returncode, timed_out, time.monotonic() - started


def build_plan(configs, repeat, extra_env, class_pools):
    """Rounds of the configurations: round r runs every configuration once, in the given order.
    extra_env (--env) is applied to every run; class_pools (--class-pools) only where the
    configuration sets SOTC_CLASS_POOLS. The plan keeps the switches each run really gets."""
    plan = []
    for rep in range(1, repeat + 1):
        for config in configs:
            switches = dict(SWITCHES[config])
            if class_pools is not None and 'SOTC_CLASS_POOLS' in switches:
                switches['SOTC_CLASS_POOLS'] = class_pools
            switches.update(extra_env)
            plan.append({'config': config, 'rep': rep, 'switches': switches})
    return plan


def run_matrix(plan, out, configs, repeat, notes):
    (out / 'logs').mkdir(parents=True, exist_ok=True)
    runs = []
    for index, item in enumerate(plan, 1):
        for attempt in range(1, RUN_ATTEMPTS + 1):  # RUN_ATTEMPTS is 2 with --retry
            log_path = out / 'logs' / f'{item["config"]}-r{item["rep"]}-a{attempt}.log'
            print(f'[{index}/{len(plan)}] {item["config"]} rep {item["rep"]} attempt {attempt} ...',
                  flush=True)
            code, timed_out, seconds = launch(item['switches'], log_path)
            record = parse_log(log_path.read_text(encoding='utf-8', errors='replace'))
            run = {'config': item['config'], 'rep': item['rep'], 'attempt': attempt,
                   'log': log_path.relative_to(out).as_posix(), 'returncode': code,
                   'timed_out': timed_out, 'seconds': seconds, 'record': record,
                   'stats': run_stats(record)}
            runs.append(run)
            status = 'ok' if record['route'] else 'route did not run'
            print(f'    {status} (exit {code}{", timed out" if timed_out else ""}, {seconds:.0f} s)',
                  flush=True)
            (out / 'summary.md').write_text(
                summary_markdown(runs, configs, 'PC memory model matrix',
                                 [f'Output: {out}', f'Repeats: {repeat}; runs so far: {len(runs)}; '
                                  f'planned runs: {len(plan)} (retries not counted).'] + notes),
                encoding='utf-8')
            if record['route']:
                break
    return runs


# --- reporting ---------------------------------------------------------------------------------------

def fmt(value, digits=1):
    return '-' if value is None else f'{value:.{digits}f}'


def config_order(runs, order):
    names = list(order)
    for run in runs:
        if run['config'] not in names:
            names.append(run['config'])
    return [name for name in names if any(r['config'] == name for r in runs)]


def aggregate(runs, order):
    """One row per configuration. Stats are means over the valid runs (final attempt of each round);
    crashes and warnings are counted over every attempt."""
    rows = []
    for name in order:
        mine = [r for r in runs if r['config'] == name]
        finals = {}
        for run in mine:
            finals[run['rep']] = run  # attempts are in order: the last one of a round wins
        final = list(finals.values())
        valid = [r for r in final if r['record']['route']]
        row = {'config': name, 'valid': len(valid), 'total': len(final),
               'crashes': sum(1 for r in mine if r['record']['crash']),
               'warnings': sum(r['record']['warnings'] for r in mine)}
        row['fps'] = mean_or_none([r['stats']['fps'] for r in valid])
        row['p99'] = mean_or_none([r['record']['route']['p99'] for r in valid])
        row['max'] = mean_or_none([r['record']['route']['max'] for r in valid])
        row['readbacks'] = mean_or_none([r['stats']['readbacks'] for r in valid])
        row['eboot'] = mean_or_none([r['stats']['eboot'] for r in valid])
        row['bpe'] = mean_or_none([r['stats']['bpe'] for r in valid])
        row['host'] = mean_or_none([r['stats']['host'] for r in valid])
        row['draw'] = mean_or_none([r['stats']['draw'] for r in valid])
        row['draw_work'] = mean_or_none([r['stats']['draw_work'] for r in valid])
        for key in ('div', 'no_extent', 'pool_matches', 'pool_mismatches', 'native_calls', 'would_crash',
                    'committed_mib'):
            row[key] = mean_or_none([r['record']['metrics'][key] for r in valid])
        units = [r['record']['metrics']['div_unit'] for r in valid if r['record']['metrics']['div_unit']]
        row['div_unit'] = units[-1] if units else None
        rows.append(row)
    return rows


def summary_markdown(runs, configs, title, notes):
    order = config_order(runs, configs)
    lines = [f'# {title}', '']
    lines += notes + ['',
                      'Means over the valid runs (the route finished). FPS, readbacks, write faults and '
                      'draw busy average the windows after boot (t > 5 s). p99 and max are the route '
                      'values, averaged over the runs. Draw work is the "SOTC DRAW WORK" line (ms per frame, '
                      'the windows after boot). Warnings count "SOTC CLASS TRIP" and "disabled '
                      'native" lines over every attempt.', '']
    lines += ['## Per configuration', '',
              '| config | valid runs | mean FPS | p99 ms | max ms | readbacks waited/s | '
              'write faults/s (eboot / bpe heap / host) | draw busy ms | draw work ms/frame | crashes | '
              'warnings |',
              '|---|---|---|---|---|---|---|---|---|---|---|']
    rows = aggregate(runs, order)
    for row in rows:
        status = f'{row["valid"]}/{row["total"]}' if row['valid'] else f'0/{row["total"]} (route did not run)'
        faults = f'{fmt(row["eboot"], 0)} / {fmt(row["bpe"], 0)} / {fmt(row["host"], 0)}'
        lines.append(f'| {row["config"]} | {status} | {fmt(row["fps"])} | {fmt(row["p99"], 2)} | '
                     f'{fmt(row["max"])} | {fmt(row["readbacks"], 0)} | {faults} | '
                     f'{fmt(row["draw"], 2)} | {fmt(row["draw_work"], 2)} | {row["crashes"]} | '
                     f'{row["warnings"]} |')
    lines += ['', '## Switch metrics (means over the valid runs)', '',
              '| config | valid runs | ring divergent | ring no-extent share % | pool matches | '
              'pool mismatches | native calls (eligible) | would-crash kills | committed MiB | '
              'readbacks waited/s |',
              '|---|---|---|---|---|---|---|---|---|---|']
    for row in rows:
        unit = f' {row["div_unit"]}' if row['div_unit'] and row['div'] is not None else ''
        divergent = f'{fmt(row["div"], 0)}{unit}'
        lines.append(f'| {row["config"]} | {row["valid"]}/{row["total"]} | {divergent} | '
                     f'{fmt(row["no_extent"], 2)} | {fmt(row["pool_matches"], 0)} | '
                     f'{fmt(row["pool_mismatches"], 0)} | {fmt(row["native_calls"], 0)} | '
                     f'{fmt(row["would_crash"], 0)} | {fmt(row["committed_mib"], 1)} | '
                     f'{fmt(row["readbacks"], 0)} |')
    lines += ['', '## Runs', '',
              '| config | round | attempt | status | frames | p50 | p99 | max | >33 | >50 | >100 | '
              'FPS | readbacks/s | DMA ms/s | draw ms | crash markers | warnings |',
              '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for run in runs:
        route = run['record']['route']
        status = 'ok' if route else 'route did not run'
        if run['timed_out']:
            status += ' (timed out)'
        cells = ['-'] * 7 if route is None else [str(route['frames']), fmt(route['p50']), fmt(route['p99']),
                                                  fmt(route['max']), str(route['hitch33']),
                                                  str(route['hitch50']), str(route['hitch100'])]
        stats = run['stats']
        lines.append(f'| {run["config"]} | {run["rep"]} | {run["attempt"]} | {status} | ' + ' | '.join(cells) +
                     f' | {fmt(stats["fps"])} | {fmt(stats["readbacks"], 0)} | {fmt(stats["dma_ms"], 1)} | '
                     f'{fmt(stats["draw"], 2)} | {len(run["record"]["crash"])} | '
                     f'{run["record"]["warnings"]} |')
    lines += ['', '## Config lines (last of each kind, per configuration)', '']
    for name in order:
        merged = {}
        pool_block = []
        for run in runs:
            if run['config'] == name:
                merged.update(run['record']['config_lines'])
                if run['record']['pool_block']:
                    pool_block = run['record']['pool_block']
        if not merged and not pool_block:
            lines.append(f'- {name}: none (no config lines in its logs).')
        for key in CONFIG_LINE_KEYS:
            if key in merged:
                lines.append(f'- {name}: `{merged[key]}`')
        for line in pool_block:
            lines.append(f'- {name} PC POOL: `{line}`')
    lines += ['', '## Warnings', '']
    warned = [(run, sample) for run in runs for sample in run['record']['warning_samples']]
    lines += [f'- {run["config"]} round {run["rep"]} attempt {run["attempt"]}: {sample}'
              for run, sample in warned] or ['None.']
    lines += ['', '## Crash markers', '']
    marked = [(run, marker) for run in runs for marker in run['record']['crash']]
    lines += [f'- {run["config"]} round {run["rep"]} attempt {run["attempt"]}: {marker}'
              for run, marker in marked] or ['None.']
    lines.append('')
    return '\n'.join(lines)


def window_table(record):
    lines = ['| t s | FPS | draw ms | draw work ms/frame | DMA ms/s | readbacks/s | eboot/s | bpe heap/s | host module/s |',
             '|---|---|---|---|---|---|---|---|---|']
    for w in record['windows']:
        lines.append(f'| {w["t"]:g} | {fmt(w["fps"])} | {fmt(w["draw"], 2)} | {fmt(w["draw_work"], 2)} | '
                     f'{fmt(w["dma_ms"])} | '
                     f'{fmt(w["readbacks"], 0)} | {fmt(w["eboot"], 0)} | {fmt(w["bpe"], 0)} | '
                     f'{fmt(w["host"], 0)} |')
    return '\n'.join(lines)


def parse_only(paths):
    runs = []
    for path in paths:
        if not path.is_file():
            raise SystemExit(f'not a file: {path}')
        record = parse_log(path.read_text(encoding='utf-8', errors='replace'))
        runs.append({'config': path.stem, 'rep': 1, 'attempt': 1, 'log': str(path), 'returncode': None,
                     'timed_out': False, 'seconds': 0.0, 'record': record, 'stats': run_stats(record)})
        print(f'## {path}: route {"done" if record["route"] else "not done"}, '
              f'{len(record["windows"])} FPS windows\n')
        print(window_table(record) + '\n')
    print(summary_markdown(runs, [r['config'] for r in runs], 'PC memory model matrix (parse only)',
                           ['Each file is one configuration, round 1.']))
    return 0


def describe_plan(plan, configs, repeat, out_text, notes):
    lines = [f'pcmem_matrix dry run: {len(plan)} run(s) = {len(configs)} configuration(s) x {repeat} '
             f'round(s), nothing launched.',
             f'Output folder (not created): {out_text}',
             f'Each run: cwd {ROOT}, {sys.executable} sotc.py, killed after {RUN_TIMEOUT_S:.0f} s; '
             f'inherited SOTC_* variables are removed.',
             f'A run without "{ROUTE_DONE}" is retried once.']
    lines += notes
    lines += ['', 'Common environment:']
    lines += [f'  {key}={value}' for key, value in COMMON_ENV.items()]
    lines += ['', 'Runs in order:']
    for index, item in enumerate(plan, 1):
        switches = ' '.join(f'{k}={v}' for k, v in item['switches'].items()) or '(none)'
        lines.append(f'  {index:>3}. {item["config"]} round {item["rep"]}: {switches}  '
                     f'-> logs/{item["config"]}-r{item["rep"]}-a1.log')
    return '\n'.join(lines)


def parse_env_items(items):
    """--env KEY=VALUE items as a dict; the key must be a plain variable name."""
    out = {}
    for item in items:
        key, sep, value = item.partition('=')
        if not sep or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', key):
            raise SystemExit(f'--env expects KEY=VALUE, got: {item}')
        out[key] = value
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--configs', nargs='+', default=DEFAULT_CONFIGS, metavar='NAME',
                        help='configurations, space or comma separated (default: ' + ', '.join(DEFAULT_CONFIGS)
                             + '); all known: ' + ', '.join(SWITCHES))
    parser.add_argument('--repeat', type=int, default=1, help='rounds: each configuration runs this many times')
    parser.add_argument('--retry', action='store_true',
                        help='retry once a run whose Lab route did not start (off: one run per configuration)')
    parser.add_argument('--out', type=Path, help='output folder (default user/pcmem/<timestamp>)')
    parser.add_argument('--env', action='append', default=[], metavar='KEY=VALUE',
                        help='add or override a variable in every run (repeatable; applied last)')
    parser.add_argument('--class-pools', metavar='NAMES',
                        help='SOTC_CLASS_POOLS for the configurations that set it (heaps); empty = game default')
    parser.add_argument('--dry-run', action='store_true', help='print the plan and launch nothing')
    parser.add_argument('--parse-only', nargs='+', type=Path, metavar='LOG',
                        help='parse existing logs (one configuration per file) and print the tables')
    args = parser.parse_args(argv)
    if args.retry:  # sotc: opt-in retry of a run whose route did not start
        global RUN_ATTEMPTS
        RUN_ATTEMPTS = 2
    if args.parse_only:
        return parse_only(args.parse_only)
    configs = []
    for chunk in args.configs:
        for name in chunk.split(','):
            name = name.strip()
            if name and name not in configs:
                configs.append(name)
    unknown = [name for name in configs if name not in SWITCHES]
    if unknown:
        parser.error('unknown configuration(s): ' + ', '.join(unknown) + ' (known: ' + ', '.join(SWITCHES) + ')')
    if not configs:
        parser.error('no configuration chosen')
    if args.repeat < 1:
        parser.error('--repeat must be at least 1')
    extra_env = parse_env_items(args.env)
    notes = []
    if extra_env:
        notes.append('Extra environment (--env, every run): ' + ' '.join(f'{k}={v}' for k, v in extra_env.items()))
    if args.class_pools is not None:
        notes.append(f'SOTC_CLASS_POOLS={args.class_pools or "(empty: game default)"} in the configurations that set it')
    plan = build_plan(configs, args.repeat, extra_env, args.class_pools)
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    if args.dry_run:
        out_text = str(args.out) if args.out else f'user/pcmem/{stamp} (default)'
        print(describe_plan(plan, configs, args.repeat, out_text, notes))
        return 0
    if not (ROOT / 'sotc.py').is_file():
        raise SystemExit(f'sotc.py not found in {ROOT}')
    out = (args.out or ROOT / 'user' / 'pcmem' / stamp).absolute()
    out.mkdir(parents=True, exist_ok=True)
    print(f'results in {out}', flush=True)
    run_matrix(plan, out, configs, args.repeat, notes)
    print(f'summary: {out / "summary.md"}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
