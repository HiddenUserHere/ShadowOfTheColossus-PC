#!/usr/bin/env python3
"""sotc: cost taxonomy of the draw thread (shadPS4:GpuDraw) over a SOTC_TRACE session.

    python tools/trace/sotc_trace_taxonomy.py <session folder> [--json out.json]

Reads every .sotctrace file of the folder (reader: tools/trace/sotc_trace.py) and de-duplicates the
frames that several dumps hold (keyed by frame start time). A frame is used only when a file's window
holds it completely. Groups: normal (unflagged, under 40 ms), mid (unflagged, 40 ms and up), slow
(flagged slow by the recorder). Prints markdown tables to stdout.

Per frame the draw thread's time is split by SELF time: a span's time minus the time of the spans
nested in it, so the parts add up to the covered time. Names and paths come from the span tree.
Spans are recorded with floors (ScopeMin / zones 20 us, OpBatch 50 us), so the time of small ops sits in
the self time of "ops" (see the caveat printed with the tables).

Categories of the self time (the UMA question: how much of the draw thread belongs to the
emulation of unified memory: coherence sync, page protect, write faults, readbacks, texture refresh):
  EMU     emulation: sync *, obtain buffer, invalidate images, upload fence, write fault, rb *, redirty,
          image upload, RefreshImage, wait upload, wait tc lock
  MIXED   bind resources / bind buffers / bind textures / find image self time (descriptor work and
          the coherence checks together), hle fill, ensure resident, wait gpu, wait recorder,
          and the untraced op body (self time of ops, op, Draw*, Dispatch*, draw, dispatch, ...)
  RENDER  real rendering work: get pipeline, render state, begin rendering, vertex buffers,
          indirect args, command (guest PM4 parsing), eop/release, func
  IDLE    draw idle
"""
import argparse
import bisect
import collections
import glob
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sotc_trace as st  # noqa: E402

LONG_OPS = ('draw', 'draw indirect', 'dispatch', 'dispatch indirect', 'func')
ZONES = ('Draw', 'DrawIndirect', 'DispatchDirect', 'DispatchIndirect')
EMU_LEAVES = {'sync copy', 'sync staging', 'sync record', 'sync protect', 'sync memory',
              'obtain buffer', 'invalidate images', 'upload fence (runtime write)', 'write fault',
              'rb begin', 'rb gather', 'rb finish', 'image upload', 'RefreshImage', 'wait upload',
              'wait tc lock', 'sync phys'}
MIXED_LEAVES = {'bind resources', 'bind buffers', 'bind textures', 'find image', 'hle fill',
                'ensure resident', 'wait gpu', 'wait recorder', 'ops', 'op', 'draw', 'draw indirect',
                'dispatch', 'dispatch indirect', 'func', 'Draw', 'DrawIndirect', 'DispatchDirect',
                'DispatchIndirect', 'redirty other'}
RENDER_LEAVES = {'get pipeline', 'render state', 'begin rendering', 'vertex buffers', 'indirect args',
                 'command', 'eop/release'}
IDLE_LEAVES = {'draw idle'}


def category(leaf):
    if leaf in IDLE_LEAVES:
        return 'IDLE'
    if leaf in EMU_LEAVES or leaf.startswith('redirty'):
        return 'EMU'
    if leaf in RENDER_LEAVES:
        return 'RENDER'
    if leaf in MIXED_LEAVES:
        return 'MIXED'
    return 'MIXED'


def build_tree(spans):
    """Self pieces (s, e, path), incl (s, e, name, path, b, a) and the count of clipped overlaps."""
    spans = sorted(spans, key=lambda e: (e.t0, -e.t1))
    selfs, incl, overlaps = [], [], 0
    stack = []  # [name, s, e, path, children]

    def finish(node):
        name, s, e, path, children = node[:5]
        cur = s
        for cs, ce in children:
            if cs > cur:
                selfs.append((cur, cs, path))
            cur = max(cur, ce)
        if e > cur:
            selfs.append((cur, e, path))

    for sp in spans:
        s, e, name = sp.t0, sp.t1, sp.name
        while stack and stack[-1][2] <= s:
            finish(stack.pop())
        parent = stack[-1] if stack else None
        if parent is not None and e > parent[2]:
            overlaps += 1
            e = parent[2]
        path = (parent[3] + (name,)) if parent is not None else (name,)
        node = [name, s, e, path, [], sp]
        if parent is not None:
            parent[4].append((s, e))
        incl.append(node)
        stack.append(node)
    while stack:
        finish(stack.pop())
    selfs.sort()
    return selfs, [(n[1], n[2], n[0], n[3], n[5]) for n in incl], overlaps


def clip_sum(items, f0, f1):
    """items: (s, e, key). Overlap with [f0, f1) per key, ns."""
    out = collections.defaultdict(float)
    for s, e, key in items:
        if e <= f0 or s >= f1:
            continue
        out[key] += min(e, f1) - max(s, f0)
    return out


def union_ns(spans, f0, f1, skip=()):
    cut = sorted((max(s, f0), min(e, f1)) for s, e, n in spans
                 if n not in skip and s < f1 and e > f0)
    total, end = 0.0, f0
    for a, b in cut:
        if b > end:
            total += b - max(a, end)
            end = b
    return total


def pct(vals, p):
    v = sorted(vals)
    if not v:
        return 0.0
    k = (len(v) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def read_session(folder):
    files = sorted(glob.glob(os.path.join(folder, '*.sotctrace')))
    frames = {}
    files_data = {}
    for path in files:
        t = st.load(path)
        w = t.meta.get('window_ns') or [0, 0]
        draw = [r for r in t.events if 'GpuDraw' in t.thread_label(r)]
        if not draw:
            continue
        dr = draw[0]
        fr_all, slow = st.slow_frames(t)
        selfs, incl, overlaps = build_tree(
            [e for e in t.events[dr] if e.kind == st.K_SPAN and e.t1 > e.t0])
        others = {}
        for r, evs in t.events.items():
            if r == dr:
                continue
            others[r] = (t.thread_label(r), t.threads.get(r, {}).get('kind'), evs)
        files_data[path] = dict(t=t, dr=dr, selfs=selfs, incl=incl, overlaps=overlaps,
                                others=others, window=w)
        for fr in fr_all:
            covered = bool(w and w[1] and w[0] <= fr.t0 and fr.t1 <= w[1])
            prev = frames.get(fr.t0)
            if prev is None or (covered and not prev['covered']):
                frames[fr.t0] = dict(t0=fr.t0, t1=fr.t1, frame=fr.a,
                                     slow=bool(fr.flags & st.FLAG_SLOW), covered=covered,
                                     file=path)
    return frames, files_data


def frame_record(fr, fd):
    f0, f1 = fr['t0'], fr['t1']
    dur = (f1 - f0) / 1e6
    self_by_path = clip_sum(fd['selfs'], f0, f1)
    self_leaf = collections.defaultdict(float)
    for p, v in self_by_path.items():
        self_leaf[p[-1]] += v
    incl = collections.defaultdict(float)
    for s, e, name, path, sp in fd['incl']:
        if e <= f0 or s >= f1:
            continue
        incl[name] += min(e, f1) - max(s, f0)
    covered = sum(self_by_path.values())
    cat = collections.defaultdict(float)
    for leaf, v in self_leaf.items():
        cat[category(leaf)] += v
    rec = dict(frame=fr['frame'], t0=f0, dur=dur, slow=fr['slow'],
               group='slow' if fr['slow'] else ('normal' if dur < 40 else 'mid'),
               covered_ms=covered / 1e6, uncovered_ms=(f1 - f0 - covered) / 1e6,
               idle_ms=self_leaf.get('draw idle', 0.0) / 1e6,
               self_leaf={k: v / 1e6 for k, v in self_leaf.items()},
               self_path={'|'.join(k): v / 1e6 for k, v in self_by_path.items()},
               incl={k: v / 1e6 for k, v in incl.items()},
               cat={k: v / 1e6 for k, v in cat.items()})
    cnt = collections.Counter()
    ops = 0
    rb_pairs = []
    begins = {}
    inst_busy = collections.Counter()
    for e in fd['t'].events[fd['dr']]:
        if not (f0 <= e.t0 < f1):
            continue
        if e.kind == st.K_SPAN:
            n = e.name
            if n in ('ops', 'op'):
                ops += e.a
                cnt['batch_spans'] += 1
                cnt['ops_in_batches'] += e.a
            elif n in LONG_OPS:
                ops += 1
                cnt['long_' + n] += 1
            elif n in ZONES:
                cnt['zone_' + n] += 1
            elif n == 'sync copy':
                cnt['copy_n'] += 1
                cnt['copy_bytes'] += e.a
                cnt['copy_bytes_aux%d' % e.aux] += e.a
            elif n == 'rb begin':
                cnt['rb_begin'] += 1
                begins[e.a] = e.t0
            elif n == 'rb finish':
                cnt['rb_finish'] += 1
                if e.a in begins:
                    rb_pairs.append((e.t1 - begins.pop(e.a)) / 1e6)
            elif n == 'rb gather':
                cnt['rb_gather'] += 1
            elif n == 'write fault':
                cnt['write_fault'] += 1
            elif n == 'obtain buffer':
                cnt['obtain_buffer'] += 1
            elif n == 'bind buffers':
                cnt['bind_buffers'] += 1
            elif n == 'bind textures':
                cnt['bind_textures'] += 1
            elif n == 'image upload':
                cnt['image_upload'] += 1
            elif n == 'find image':
                cnt['find_image'] += 1
            elif n == 'begin rendering':
                cnt['begin_rendering'] += 1
            elif n == 'eop/release':
                cnt['eop'] += 1
            elif n == 'wait gpu':
                cnt['wait_gpu'] += 1
        elif e.kind == st.K_INSTANT:
            if e.name == 'hle fill':
                cnt['hle_fill'] += 1
            elif e.name.startswith('redirty'):
                cnt['redirty'] += 1
                cnt['redirty_bytes'] += e.a
    cnt['ops_total'] = ops
    rec['counts'] = dict(cnt)
    rec['rb_chain_ms'] = rb_pairs
    rec['rb_open'] = len(begins)
    # Other threads: busy time of each ring inside the frame.
    other = {}
    for r, (lab, kind, evs) in fd['others'].items():
        sp = [(e.t0, e.t1, e.name) for e in evs if e.kind == st.K_SPAN and e.t1 > e.t0]
        gpu = [(e.t0, e.t1, e.name) for e in evs if e.kind == st.K_GPU and e.t1 > e.t0]
        if kind == 'cp':
            other['cp_busy_ms'] = other.get('cp_busy_ms', 0) + union_ns(sp, f0, f1, ('cp idle',)) / 1e6
        elif kind == 'recorder':
            other['rec_busy_ms'] = other.get('rec_busy_ms', 0) + union_ns(sp, f0, f1) / 1e6
        elif kind == 'gpu' and 'Present' in lab:
            other['present_ms'] = other.get('present_ms', 0) + union_ns(sp, f0, f1) / 1e6
        elif kind == 'queue':
            other['gpu_watch_ms'] = other.get('gpu_watch_ms', 0) + union_ns(sp, f0, f1) / 1e6
        elif kind == 'gpu':
            other['gpu_cmdbuf_ms'] = other.get('gpu_cmdbuf_ms', 0) + union_ns(gpu, f0, f1) / 1e6
        elif kind == 'guest':
            w = collections.Counter()
            for e in evs:
                if e.kind == st.K_SPAN and e.t1 > e.t0 and e.t0 < f1 and e.t1 > f0:
                    w[e.name] += min(e.t1, f1) - max(e.t0, f0)
            other.setdefault('guest', {})[lab] = {k: v / 1e6 for k, v in w.items()}
    rec['other'] = other
    return rec


def gather(folder):
    frames, files_data = read_session(folder)
    recs = []
    for key in sorted(frames):
        fr = frames[key]
        if not fr['covered']:
            continue
        recs.append(frame_record(fr, files_data[fr['file']]))
    return recs, files_data


def stats(vals):
    if not vals:
        return {'mean': 0.0, 'p50': 0.0, 'p95': 0.0, 'n': 0}
    return {'mean': statistics.fmean(vals), 'p50': pct(vals, .5), 'p95': pct(vals, .95),
            'n': len(vals)}


def md_table(header, rows):
    out = ['| ' + ' | '.join(header) + ' |', '|' + '|'.join('---' for _ in header) + '|']
    out += ['| ' + ' | '.join(str(c) for c in r) + ' |' for r in rows]
    return out


def fmt(x, d=2):
    return f'{x:.{d}f}'


def tables(recs):
    G = collections.defaultdict(list)
    for r in recs:
        G[r['group']].append(r)
    groups = ['normal', 'mid', 'slow']
    out = []
    out.append('## Frame groups (covered frames only)')
    out.append('')
    rows = []
    for g in groups:
        fs = G[g]
        rows.append([g, len(fs)] + [f"{stats([f[k] for f in fs])['mean']:.1f} "
                                    f"({stats([f[k] for f in fs])['p50']:.1f} / "
                                    f"{stats([f[k] for f in fs])['p95']:.1f})"
                                    for k in ('dur',)] +
                    [fmt(stats([f['covered_ms'] for f in fs])['mean'], 1),
                     fmt(stats([f['idle_ms'] for f in fs])['mean'], 2),
                     fmt(stats([f['uncovered_ms'] for f in fs])['mean'], 1),
                     fmt(stats([f['counts'].get('ops_total', 0) for f in fs])['mean'], 0)])
    out += md_table(['group', 'n', 'frame ms mean (p50 / p95)', 'draw covered ms', 'draw idle ms',
                     'uncovered ms', 'ops per frame'], rows)
    out.append('')
    # Category table
    out.append('## Draw thread self time by category (ms per frame)')
    out.append('')
    rows = []
    cats = ['EMU', 'MIXED', 'RENDER', 'IDLE']
    base = {}
    for c in cats:
        row = [c]
        for g in groups:
            vals = [f['cat'].get(c, 0.0) for f in G[g]]
            s = stats(vals)
            row.append(f"{s['mean']:.2f} ({s['p50']:.2f} / {s['p95']:.2f})")
            base.setdefault(c, {})[g] = s['mean']
        row.append(f"{base[c]['slow'] - base[c]['normal']:+.2f}")
        rows.append(row)
    out += md_table(['category', 'normal mean (p50 / p95)', 'mid mean (p50 / p95)',
                     'slow mean (p50 / p95)', 'slow - normal'], rows)
    emu_share = {g: base['EMU'][g] / max(1e-9, sum(base[c][g] for c in cats if c != 'IDLE'))
                 for g in groups}
    out.append('')
    out.append('EMU share of busy draw time (EMU + MIXED + RENDER): ' +
               ', '.join(f"{g} {emu_share[g]:.0%}" for g in groups) + '.')
    out.append('')
    # Self by leaf
    leaves = collections.defaultdict(lambda: collections.defaultdict(list))
    for g in groups:
        for f in G[g]:
            keys = set(f['self_leaf'])
            for k in keys:
                leaves[k][g].append(f['self_leaf'][k])
            for k in list(leaves):
                if k not in keys:
                    leaves[k][g].append(0.0)
    rows = []
    for k in sorted(leaves, key=lambda k: -(stats(leaves[k]['slow'])['mean'] -
                                            stats(leaves[k]['normal'])['mean'])):
        m = {g: stats(leaves[k][g])['mean'] for g in groups}
        if max(m.values()) < 0.005:
            continue
        rows.append([k, category(k), fmt(m['normal']), fmt(m['mid']), fmt(m['slow']),
                     f"{m['slow'] - m['normal']:+.2f}",
                     fmt(stats(leaves[k]['slow'])['p95']) if leaves[k]['slow'] else '0'])
    out.append('## Draw thread self time by span name (ms per frame, mean over the group)')
    out.append('')
    out += md_table(['span (self)', 'category', 'normal', 'mid', 'slow', 'slow - normal',
                     'slow p95'], rows)
    out.append('')
    return out, G, base


def inclusive_table(G):
    groups = ['normal', 'mid', 'slow']
    names = ['ops', 'command', 'bind resources', 'bind buffers', 'bind textures', 'obtain buffer',
             'sync memory', 'sync copy', 'sync protect', 'sync record', 'sync staging',
             'find image', 'image upload', 'render state', 'begin rendering', 'get pipeline',
             'rb begin', 'rb finish', 'rb gather', 'wait gpu', 'wait tc lock', 'wait recorder',
             'wait upload', 'write fault', 'draw idle', 'Draw', 'DrawIndirect', 'DispatchDirect',
             'DispatchIndirect', 'draw', 'draw indirect', 'dispatch', 'dispatch indirect', 'func']
    rows = []
    for n in names:
        m = {g: stats([f['incl'].get(n, 0.0) for f in G[g]])['mean'] for g in groups}
        if max(m.values()) < 0.005:
            continue
        rows.append([n, fmt(m['normal']), fmt(m['mid']), fmt(m['slow']),
                     f"{m['slow'] - m['normal']:+.2f}"])
    out = ['## Inclusive time per span name (ms per frame; nested names overlap, do not add up)', '']
    out += md_table(['span', 'normal', 'mid', 'slow', 'slow - normal'], rows)
    return out


def counts_table(G):
    groups = ['normal', 'mid', 'slow']
    keys = ['ops_total', 'ops_in_batches', 'long_draw', 'long_draw indirect', 'long_dispatch',
            'long_dispatch indirect', 'zone_Draw', 'zone_DrawIndirect', 'zone_DispatchDirect',
            'zone_DispatchIndirect', 'copy_n', 'copy_bytes', 'rb_begin', 'rb_finish', 'rb_gather',
            'write_fault', 'obtain_buffer', 'bind_buffers', 'bind_textures', 'find_image',
            'image_upload', 'redirty', 'hle_fill', 'wait_gpu']
    rows = []
    for k in keys:
        m = {g: stats([f['counts'].get(k, 0) for f in G[g]])['mean'] for g in groups}
        rows.append([k] + [fmt(m[g], 1) for g in groups])
    out = ['## Counts per frame (mean)', '']
    out += md_table(['count', 'normal', 'mid', 'slow'], rows)
    return out


def copy_table(G):
    groups = ['normal', 'mid', 'slow']
    rows = []
    keys = sorted({k for g in groups for f in G[g] for k in f['counts'] if k.startswith('copy_bytes_aux')})
    names = {'copy_bytes_aux0': 'untagged', 'copy_bytes_aux1': 'bind', 'copy_bytes_aux2': 'written',
             'copy_bytes_aux3': 'dma', 'copy_bytes_aux4': 'SynchronizeBinding (code 4)',
             'copy_bytes_aux5': 'WarmRange (code 5)'}
    for k in keys:
        m = {g: stats([f['counts'].get(k, 0) for f in G[g]]) for g in groups}
        rows.append([names.get(k, k), *[f"{m[g]['mean'] / 2**20:.2f} MiB (p50 {m[g]['p50'] / 2**20:.2f})"
                                       for g in groups]])
    out = ['## Sync copy bytes per frame by reason (mean MiB, p50)', '']
    out += md_table(['reason', 'normal', 'mid', 'slow'], rows)
    return out


def rb_table(G):
    groups = ['normal', 'mid', 'slow']
    rows = []
    for g in groups:
        chains = [x for f in G[g] for x in f['rb_chain_ms']]
        rows.append([g, len(G[g]), fmt(stats([f['counts'].get('rb_begin', 0) for f in G[g]])['mean'], 1),
                     fmt(stats([f['counts'].get('rb_finish', 0) for f in G[g]])['mean'], 1),
                     fmt(stats([f['self_leaf'].get('rb finish', 0) for f in G[g]])['mean'], 2),
                     fmt(stats([f['self_leaf'].get('rb begin', 0) for f in G[g]])['mean'], 2),
                     fmt(stats([f['incl'].get('wait gpu', 0) for f in G[g]])['mean'], 2),
                     len(chains), fmt(pct(chains, .5), 2) if chains else '-', fmt(pct(chains, .95), 2) if chains else '-'])
    out = ['## Readback chains (draw thread)', '']
    out += md_table(['group', 'frames', 'rb begin / frame', 'rb finish / frame', 'self rb finish ms',
                     'self rb begin ms', 'wait gpu inclusive ms', 'chains paired', 'chain p50 ms',
                     'chain p95 ms'], rows)
    return out


def other_table(G):
    groups = ['normal', 'mid', 'slow']
    keys = ['cp_busy_ms', 'rec_busy_ms', 'present_ms', 'gpu_watch_ms', 'gpu_cmdbuf_ms']
    rows = []
    for k in keys:
        m = {g: stats([f['other'].get(k, 0.0) for f in G[g]]) for g in groups}
        rows.append([k] + [f"{m[g]['mean']:.2f} (p50 {m[g]['p50']:.2f})" for g in groups])
    out = ['## Other threads per frame (busy ms, union of recorded spans)', '']
    out += md_table(['thread', 'normal', 'mid', 'slow'], rows)
    out.append('')
    out.append('Game threads: time per wait kind inside the frame (mean ms, slow frames):')
    out.append('')
    guest = collections.defaultdict(lambda: collections.defaultdict(list))
    for f in G['slow']:
        for lab, w in f['other'].get('guest', {}).items():
            for k, v in w.items():
                guest[lab][k].append(v)
    rows = []
    for lab in sorted(guest):
        top = sorted(guest[lab].items(), key=lambda kv: -statistics.fmean(kv[1]))[:4]
        rows.append([lab, '; '.join(f"{k} {statistics.fmean(v):.1f}" for k, v in top)])
    out += md_table(['thread', 'top span kinds (mean ms per slow frame)'], rows)
    return out


def drivers(G, base):
    """Ranked drivers: slow-normal delta, and the frame time left if the driver were free."""
    groups = ['normal', 'mid', 'slow']
    names = collections.defaultdict(lambda: collections.defaultdict(list))
    for g in groups:
        for f in G[g]:
            for k, v in f['self_leaf'].items():
                names[k][g].append(v)
    entries = []
    slow_dur = stats([f['dur'] for f in G['slow']])['mean']
    for k in names:
        m = {g: stats(names[k][g] + [0.0] * (len(G[g]) - len(names[k][g])))['mean'] for g in groups}
        delta = m['slow'] - m['normal']
        if max(m.values()) < 0.05 or k == 'draw idle':
            continue
        entries.append((delta, k, m))
    entries.sort(reverse=True)
    rows = []
    for delta, k, m in entries[:20]:
        rows.append([k, category(k), fmt(m['normal']), fmt(m['slow']), f"{delta:+.2f}",
                     fmt(slow_dur - m['slow'], 1)])
    out = ['## Ranked cost drivers (draw thread self ms per frame)', '',
           'Floor = mean slow frame time minus the driver\'s mean slow time (frame time if the span cost were zero, '
           'ignoring knock-on effects).', '']
    out += md_table(['driver (self span)', 'category', 'normal ms', 'slow ms', 'slow - normal',
                     'slow frame floor ms'], rows)
    return out


def groups_floor(G):
    """The UMA split as floors: time left if one class of self time were free (slow and normal)."""
    groups = ['normal', 'mid', 'slow']
    out = ['## Floors by class (mean ms per frame; left = frame ms minus the class)', '']
    rows = []
    for c in ('EMU', 'MIXED', 'RENDER', 'IDLE'):
        m = {g: stats([f['cat'].get(c, 0.0) for f in G[g]])['mean'] for g in groups}
        dur = {g: stats([f['dur'] for f in G[g]])['mean'] for g in groups}
        rows.append([c] + [f"{m[g]:.1f} -> left {dur[g] - m[g]:.1f}" for g in groups])
    out += md_table(['class (traced self time)', 'normal', 'mid', 'slow'], rows)
    out.append('')
    out.append('MIXED includes the untraced op body (self time of "ops" and of the op and zone wrappers); '
               'its split is not in the trace. Estimates for it are in the report, not here.')
    return out


def callers(folder, files_data, G_slow_keys):
    """Guest and host callers of the readback chain and the waits that overlap it, for the frames
    in G_slow_keys (set of frame start ns). Labels come from the symbolizer of each file."""
    wait_names = ('evf wait', 'cond wait', 'sema wait', 'equeue wait', 'sceKernelWaitEqueue',
                  'cond timedwait', 'mutex wait', 'sleep', 'file read', 'read fault', 'write fault',
                  'readback')
    by_rb_waiter = collections.Counter()
    rb_ms = 0.0
    by_guest_fault = collections.Counter()
    by_draw_fault = collections.Counter()
    for path, fd in files_data.items():
        t = fd['t']
        sym = st.make_symbolizer(t)
        dr = fd['dr']
        finishes = [e for e in t.events[dr] if e.name == 'rb finish' and e.t1 > e.t0
                    and any(f0 <= e.t0 < f1 for f0, f1 in G_slow_keys)]
        waits = []
        for r, evs in t.events.items():
            if r == dr:
                continue
            kind = t.threads.get(r, {}).get('kind')
            if kind != 'guest':
                continue
            for e in evs:
                if e.t1 > e.t0 and e.name in wait_names:
                    waits.append(e)
                    if e.name in ('read fault', 'write fault', 'readback'):
                        lbl = st._caller_label(e.b, sym) if e.b else '-'
                        by_guest_fault[(t.thread_label(r), e.name, lbl)] += (e.t1 - e.t0) / 1e6
        waits.sort(key=lambda e: e.t0)
        starts = [w.t0 for w in waits]
        for fin in finishes:
            rb_ms += (fin.t1 - fin.t0) / 1e6
            lo = bisect.bisect_left(starts, fin.t0 - 10**9)
            for w in waits[lo:]:
                if w.t0 >= fin.t1:
                    break
                if w.t1 <= fin.t0:
                    continue
                ov = (min(w.t1, fin.t1) - max(w.t0, fin.t0)) / 1e6
                by_rb_waiter[(t.thread_label(w.thread), w.name,
                              st._caller_label(w.b, sym) if w.b else '-')] += ov
        for e in t.events[dr]:
            if e.name == 'write fault' and e.t1 >= e.t0 and any(f0 <= e.t0 < f1 for f0, f1 in G_slow_keys):
                by_draw_fault[st._caller_label(e.b, sym) if e.b else '-'] += 1
    out = ['## Readback callers (slow frames)', '',
           f'rb finish time inside slow frames: {rb_ms:.1f} ms in total. Guest waits that overlap it '
           '(thread, wait kind, guest caller), biggest first:', '']
    rows = [[a, b, c, f'{ms:.1f}'] for (a, b, c), ms in by_rb_waiter.most_common(10)]
    out += md_table(['waiting thread', 'wait kind', 'caller', 'overlap ms'], rows)
    out += ['', 'Guest-side readback and fault spans (slow frames; caller = b field):', '']
    rows = [[a, b, c, f'{ms:.1f}'] for (a, b, c), ms in by_guest_fault.most_common(10)]
    out += md_table(['thread', 'span', 'caller', 'ms over all files'], rows)
    out += ['', 'Draw-thread write faults in slow frames by host RIP (count):', '']
    rows = [[c, n] for c, n in by_draw_fault.most_common(8)]
    out += md_table(['host site', 'count'], rows)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--json', default=None)
    args = ap.parse_args(argv)
    recs, files_data = gather(args.folder)
    overlaps = sum(fd['overlaps'] for fd in files_data.values())
    out = [f'Frames covered: {len(recs)}; clipped nested overlaps: {overlaps}', '']
    t, G, base = tables(recs)
    out += t
    out += inclusive_table(G)
    out.append('')
    out += counts_table(G)
    out.append('')
    out += copy_table(G)
    out.append('')
    out += rb_table(G)
    out.append('')
    out += other_table(G)
    out.append('')
    out += drivers(G, base)
    out.append('')
    out += groups_floor(G)
    out.append('')
    slow_keys = [(f['t0'], f['t0'] + f['dur'] * 1e6) for f in G['slow']]
    out += callers(args.folder, files_data, slow_keys)
    print('\n'.join(out))
    if args.json:
        with open(args.json, 'w') as fh:
            json.dump(recs, fh)


if __name__ == '__main__':
    main()
