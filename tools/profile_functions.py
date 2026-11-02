#!/usr/bin/env python3
"""sotc: per-function self time (leaf samples) of a SOTC_PROFILE_OUT dump:
profile_functions.py dump [exe] [top] [--inlines] [--qualified]
--inlines: an address counts for every function inlined at it (self time for the innermost one),
so code inlined into big callers (LTO/PGO builds) shows under its own name.
--qualified: Namespace::Class::function names instead of short ones (lambdas: operator() of ...).
--callers NAME [--depth N]: who calls the functions whose name contains NAME (substring of the
name without its parameter list; one group per matching function). For each direct call site
(E8 call rel32 to the function's entry, found by scanning exe's code sections) the dump's
inclusive count of the return address is the number of samples with that call active, i.e. the
samples of NAME that come through that site (a return address is on the stack exactly while the
call it belongs to runs). Tail jumps (E9 to the entry, compiled as "return NAME(...)") leave no
return address, so they are listed separately with the function that jumps. --depth 2 adds the
callers of each top caller; those counts cover all of that caller's samples, not only the ones
that reach NAME, because the dump keeps no stacks.
--callees NAME: the reverse, for each matching function's own body (its .pdata range, disassembled
with llvm-objdump): the direct calls out of it, each with the samples under that call, and the
share left in its own code (inlined code, and time in calls it makes through pointers).
exe: the bb-probe.exe the dump came from. The dump holds bb-probe.exe offsets, so use the build of
that run. When omitted: <dump>.exe if it exists (tools/sotc_run.sh keeps it there when
SOTC_PROFILE_OUT is set), else out/gpu/bb-probe.exe, which is the newest build and not necessarily
the one that wrote the dump. --callers needs llvm-nm and llvm-symbolizer on PATH (CLANG64 bin)."""
import collections, mmap, os, re, struct, subprocess, sys

IMAGE_BASE = 0x140000000  # the dump's offsets are RVAs from this base (see the addrs line below)

argv = sys.argv[1:]
flags, args, callers, callees, depth = set(), [], None, None, 1
i = 0
while i < len(argv):
    a = argv[i]
    if a == '--callers' and i + 1 < len(argv):
        callers = argv[i + 1]; i += 2; continue
    if a.startswith('--callers='):
        callers = a.split('=', 1)[1]; i += 1; continue
    if a == '--depth' and i + 1 < len(argv):
        depth = int(argv[i + 1]); i += 2; continue
    if a == '--callees' and i + 1 < len(argv):
        callees = argv[i + 1]; i += 2; continue
    if a.startswith('--'):
        flags.add(a)
    else:
        args.append(a)
    i += 1
if len(args) > 1:
    exe = args[1]
elif os.path.isfile(args[0] + '.exe'):
    exe = args[0] + '.exe'
else:
    exe = 'out/gpu/bb-probe.exe'
print(f'symbols from: {exe}', file=sys.stderr)
top = int(args[2]) if len(args) > 2 else 45
inlines = '--inlines' in flags
qualified = '--qualified' in flags
rows, samples = [], 1
for line in open(args[0], encoding='utf-8', errors='replace'):
    parts = line.split()
    if parts[0] == 'samples':
        samples = int(parts[1]); continue
    rows.append((' '.join(parts[:-2]), int(parts[-2]), int(parts[-1])))
own = [r for r in rows if r[0].startswith('bb-probe.exe+')]


def short(name):
    """Drops parameter lists and template arguments so names stay readable."""
    if not qualified:
        return name
    out, depth_ = [], 0
    for ch in name:
        if ch in '<(':
            depth_ += 1
        elif ch in '>)':
            depth_ -= 1
        elif depth_ == 0:
            out.append(ch)
    return re.sub(r'\s+', ' ', ''.join(out)).strip()


def plain(name):
    """Namespace::Class::function without parameters, template arguments or cv-qualifiers."""
    name = name.replace('operator()', 'operator@@')
    out, depth_ = [], 0
    for ch in name:
        if ch in '<(':
            depth_ += 1
        elif ch in '>)':
            depth_ -= 1
        elif depth_ == 0:
            out.append(ch)
    text = ''.join(out).replace('operator@@', 'operator()')
    text = re.sub(r'\s+(const|volatile|&&|&)$', '', text.strip())
    return re.sub(r'\s+', ' ', text).strip()


def symbolize(addresses):
    """Function name (plain) of each VA; '??' when unknown. Same llvm-symbolizer as the profile."""
    names = []
    for i in range(0, len(addresses), 2000):
        cmd = ['llvm-symbolizer', f'--obj={exe}', '--functions=linkage', '--no-inlines', '--demangle']
        out = subprocess.run(cmd + [hex(a) for a in addresses[i:i + 2000]], capture_output=True,
                             text=True).stdout.strip().split('\n\n')
        for block in out:
            lines = block.split('\n')
            names.append(plain(lines[0]) if lines and lines[0] else '??')
    return names


def nm_functions():
    """(VA, plain name) of every defined function symbol of exe (llvm-nm, demangled)."""
    out = subprocess.run(['llvm-nm', '-C', '--defined-only', exe], capture_output=True, text=True)
    funcs = []
    for line in out.stdout.split('\n'):
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in ('T', 't'):
            funcs.append((int(parts[0], 16), plain(parts[2])))
    return funcs


def pe_layout(mm):
    """(ImageBase, [(section name, RVA, raw pointer, raw size, characteristics)]) of a PE32+ image."""
    pe = struct.unpack_from('<I', mm, 0x3C)[0]
    coff = pe + 4
    nsec = struct.unpack_from('<H', mm, coff + 2)[0]
    optsz = struct.unpack_from('<H', mm, coff + 16)[0]
    opt = coff + 20
    base = struct.unpack_from('<Q', mm, opt + 24)[0]
    sections = []
    for n in range(nsec):
        s = opt + optsz + 40 * n
        name = mm[s:s + 8].rstrip(b'\0').decode('ascii', 'replace')
        _, va, rawsz, rawptr = struct.unpack_from('<IIII', mm, s + 8)
        chars = struct.unpack_from('<I', mm, s + 36)[0]
        sections.append((name, va, rawptr, rawsz, chars))
    return base, sections


def scan_calls(targets):
    """Finds E8 (call) and E9 (jmp) rel32 instructions whose target is one of targets (VA -> label)
    in the code sections of exe. Returns (opcode, instruction VA, label) tuples."""
    hits = []
    with open(exe, 'rb') as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            base, sections = pe_layout(mm)
            for _, va, rawptr, rawsz, chars in sections:
                if not chars & 0x20:  # IMAGE_SCN_CNT_CODE
                    continue
                raw = mm[rawptr:rawptr + rawsz]
                for m in re.finditer(b'[\xe8\xe9]', raw):
                    k = m.start()
                    if k + 5 > len(raw):
                        break
                    instr = base + va + k
                    target = instr + 5 + struct.unpack_from('<i', raw, k + 1)[0]
                    if target in targets:
                        hits.append((raw[k], instr, targets[target]))
        finally:
            mm.close()
    return hits


def function_range(entry):
    """(begin, end) VA of the function starting at entry, from the exception directory (.pdata)."""
    with open(exe, 'rb') as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            base, sections = pe_layout(mm)
            for name, _, rawptr, rawsz, _ in sections:
                if name != '.pdata':
                    continue
                for off in range(rawptr, rawptr + rawsz - 11, 12):
                    begin, end = struct.unpack_from('<II', mm, off)
                    if base + begin == entry:
                        return entry, base + end
        finally:
            mm.close()
    return None


def disassemble(start, stop):
    """(VA, text) of each instruction in [start, stop) (llvm-objdump, demangled call targets)."""
    out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '--demangle',
                          f'--start-address={hex(start)}', f'--stop-address={hex(stop)}', exe],
                         capture_output=True, text=True).stdout
    insts = []
    for line in out.split('\n'):
        m = re.match(r'^([0-9a-f]+):\s+(\S.*)$', line)
        if m:
            insts.append((int(m.group(1), 16), m.group(2).strip()))
    return insts


def callees_report(needle):
    """Where the time of each matching function goes: its direct calls, each with the dump's count
    of samples under that call (the inclusive count of the return address after the call)."""
    funcs = nm_functions()
    groups = collections.defaultdict(list)
    for va, name in funcs:
        if needle in name:
            groups[name].append(va)
    if not groups:
        print(f'no function name contains {needle!r} in {exe}')
        return
    inc_at = {}
    for where, leaf, inc in own:
        inc_at[int(where.split('+')[1], 16)] = inc
    print(f'{samples} samples; direct callees of functions matching {needle!r}:')
    for name, vas in sorted(groups.items()):
        for entry in vas:
            total = sum(inc_at.get(instr + 5 - IMAGE_BASE, 0)
                        for op, instr, _ in scan_calls({entry: name}) if op == 0xE8)
            rng = function_range(entry)
            if not rng:
                print(f'{name}: no .pdata range at {hex(entry)}')
                continue
            insts = disassemble(*rng)
            per_callee = collections.defaultdict(lambda: [0, 0])  # callee -> [sites, inc]
            indirect = 0
            for idx, (addr, text) in enumerate(insts):
                if not text.startswith('call'):
                    continue
                m = re.match(r'call[qlw]?\s+0x([0-9a-f]+)\s*<(.*)>\s*$', text)
                if not m or idx + 1 >= len(insts):
                    indirect += 1
                    continue
                callee = plain(m.group(2))
                per_callee[callee][0] += 1
                per_callee[callee][1] += inc_at.get(insts[idx + 1][0] - IMAGE_BASE, 0)
            called = sum(v[1] for v in per_callee.values())
            # Samples under a function that is tail-jumped into have no direct call site here, so the
            # callee counts can exceed the direct-call total: then the callee total is the denominator.
            total = max(total, called)
            print(f'{name} at {hex(entry)} ({hex(rng[0])}-{hex(rng[1])}): {100 * total / samples:.1f}%'
                  f' of samples; {sum(v[0] for v in per_callee.values())} direct call sites,'
                  f' {indirect} indirect/unresolved')
            for callee, (sites, n) in sorted(per_callee.items(), key=lambda kv: -kv[1][1])[:25]:
                if n == 0:
                    continue
                print(f'  {100 * n / samples:5.1f}% of samples  {100 * n / total if total else 0:5.1f}%'
                      f' of its calls  {sites:2d} site(s)  {callee}')
            print(f'  {100 * (total - called) / samples if total else 0:5.1f}% of samples'
                  f' in its own code (not in a direct call; includes inlined code and indirect calls)')


def callers_report(needle):
    funcs = nm_functions()
    groups = collections.defaultdict(list)  # plain name -> entry VAs
    for va, name in funcs:
        if needle in name:
            groups[name].append(va)
    if not groups:
        print(f'no function name contains {needle!r} in {exe}')
        return
    inc_at = {}
    for where, leaf, inc in own:
        inc_at[int(where.split('+')[1], 16)] = inc  # dump offset == RVA
    targets = {va: name for name, vas in groups.items() for va in vas}
    hits = scan_calls(targets)
    site_names = dict(zip([h[1] for h in hits], symbolize([h[1] for h in hits])))

    def report(group_sites, indent):
        """group_sites: label -> list of (opcode, instr VA). Prints each caller with its share."""
        for label, sites in group_sites.items():
            calls = [s for s in sites if s[0] == 0xE8]
            tails = [s for s in sites if s[0] == 0xE9]
            total = sum(inc_at.get(s[1] + 5 - IMAGE_BASE, 0) for s in calls)
            by_caller = collections.defaultdict(lambda: [0, 0])  # caller -> [sites, inc]
            for _, instr in calls:
                caller = site_names.get(instr, '??')
                by_caller[caller][0] += 1
                by_caller[caller][1] += inc_at.get(instr + 5 - IMAGE_BASE, 0)
            print(f'{indent}{label}: {len(calls)} direct call sites, {len(tails)} tail-jump sites;'
                  f' {100 * total / samples:.1f}% of samples come through a direct call')
            for caller, (count, n) in sorted(by_caller.items(), key=lambda kv: -kv[1][1]):
                if n == 0 and count == 0:
                    continue
                share = 100 * n / total if total else 0.0
                print(f'{indent}  {share:5.1f}% of its calls  {100 * n / samples:5.1f}% of samples'
                      f'  {count:2d} site(s)  {caller}')
            for _, instr in tails:
                print(f'{indent}  tail jump from {site_names.get(instr, "??")} (no return address;'
                      f' the samples sit under that function\'s callers)')
    print(f'{samples} samples; callers of functions matching {needle!r}:')
    matched = {}
    for name, vas in sorted(groups.items()):
        sites = [(op, instr) for op, instr, label in hits if label == name]
        matched[name] = sites
        report({name: sites}, '')
    if depth >= 2:
        print('second level (callers of the top callers; all samples of that caller):')
        seen = set()
        for name, sites in matched.items():
            callers_top = collections.Counter()
            for op, instr in sites:
                if op == 0xE8:
                    callers_top[site_names.get(instr, '??')] += inc_at.get(instr + 5 - IMAGE_BASE, 0)
            for caller, n in callers_top.most_common(4):
                if caller == '??' or caller in seen or n == 0:
                    continue
                seen.add(caller)
                caller_vas = [va for va, nm in funcs if nm == caller]
                if not caller_vas:
                    print(f'  {caller}: no entry symbol')
                    continue
                caller_targets = {va: caller for va in caller_vas}
                second = scan_calls(caller_targets)
                names2 = dict(zip([h[1] for h in second], symbolize([h[1] for h in second])))
                print(f'  {caller} ({100 * n / samples:.1f}% of samples via {name}):')
                by_caller2 = collections.defaultdict(lambda: [0, 0])
                for op2, instr2, _ in second:
                    if op2 != 0xE8:
                        continue
                    by_caller2[names2.get(instr2, '??')][0] += 1
                    by_caller2[names2.get(instr2, '??')][1] += inc_at.get(instr2 + 5 - IMAGE_BASE, 0)
                for c2, (count2, n2) in sorted(by_caller2.items(), key=lambda kv: -kv[1][1])[:6]:
                    print(f'      {100 * n2 / samples:5.1f}% of samples  {count2:2d} site(s)  {c2}')


if callers is not None:
    callers_report(callers)
    sys.exit(0)
if callees is not None:
    callees_report(callees)
    sys.exit(0)

chains = []  # per address: function names, innermost first
addrs = [hex(IMAGE_BASE + int(r[0].split('+')[1], 16)) for r in own]
for i in range(0, len(addrs), 2000):
    cmd = ['llvm-symbolizer', f'--obj={exe}', '--functions=linkage' if qualified else '--functions=short',
           '--inlines' if inlines else '--no-inlines', '--demangle']
    out = subprocess.run(cmd + addrs[i:i + 2000], capture_output=True, text=True).stdout.strip().split('\n\n')
    for block in out:
        lines = block.split('\n')
        chains.append([short(lines[j]) for j in range(0, len(lines), 2)] or ['??'])
self_time = collections.Counter()
incl = collections.Counter()
for (where, leaf, inc), chain in zip(own, chains):
    self_time[chain[0]] += leaf
    seen = set()
    for depth_, name in enumerate(chain):
        if name in seen:
            continue
        seen.add(name)
        # The leaf address's innermost frame is self time; every other frame here is a caller.
        incl[name] += inc if depth_ > 0 else inc - leaf
other = collections.Counter()
for where, leaf, inc in rows:
    if not where.startswith('bb-probe.exe+'):
        other[re.sub(r'\+0x[0-9a-f]+$', '', where)] += leaf
print(f'{samples} samples; self time by function:')
for name, n in self_time.most_common(40):
    print(f'  {100*n/samples:5.1f}%  {name[:110]}')
print('outside the loader (by module):')
for name, n in other.most_common(10):
    print(f'  {100*n/samples:5.1f}%  {name}')
print('inclusive (callees) by function:')
for name, n in incl.most_common(top):
    print(f'  {100*n/samples:5.1f}%  {name[:110]}')
