import re, subprocess, sys
lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
s = lines.index('STACKS: begin'); e = lines.index('STACKS: end', s)
threads = []; cur = None
for l in lines[s+1:e]:
    m = re.match(r"thread (\d+) '(.*)': rip (\S+)", l)
    if m: cur = [m.group(2) or m.group(1), m.group(3), []]; threads.append(cur); continue
    m = re.search(r'exe (0x[0-9a-f]+)', l)
    if m and cur: cur[2].append(m.group(1))
addrs = sorted({a for t in threads for a in t[2]})
out = subprocess.run(['C:/msys64/clang64/bin/llvm-symbolizer.exe', '--obj=out/gpu/bb-probe.exe', '--functions=short'] + addrs, capture_output=True, text=True).stdout.strip().split('\n\n')
sym = {}
for a, blk in zip(addrs, out):
    ls = blk.split('\n'); sym[a] = (ls[0][:70] + ' ' + ls[1].replace(chr(92),'/').split('/')[-1][:40]) if len(ls) > 1 else ls[0]
for name, rip, fr in threads:
    seen = []
    for a in fr:
        f = sym.get(a, a)
        if f not in seen: seen.append(f)
    if not seen: continue
    print(f'== {name}  rip {rip}')
    for f in seen[:int(sys.argv[2])]: print('   ', f)
