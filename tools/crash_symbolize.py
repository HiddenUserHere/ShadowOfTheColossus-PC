"""sotc: symbolize the host stacks of a crash log: crash_symbolize.py <log> [exe] [--append]

Finds the loader addresses the crash report prints (exe+0x<offset> from the GPU stop, bb-probe.exe+0x<offset>
from the CPU fault chain, "exe 0x<address>" from the STACKS dump of every thread) and resolves them with
llvm-symbolizer against the bb-probe.exe of the same build (default out/gpu/bb-probe.exe, ImageBase 0x140000000).
Prints "<address> <function> <file>:<line>" per address; --append adds that table to the end of the log instead.
sotc.py runs it on the copy in user/crashes. Needs llvm-symbolizer (MSYS2 CLANG64); without it, or without the
exe, it does nothing (packaged builds)."""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

IMAGE_BASE = 0x140000000
PATTERNS = (
    (re.compile(r'\bexe\+0x([0-9a-f]+)'), True),           # GPU stop: offset
    (re.compile(r'bb-probe\.exe\+0x([0-9a-f]+)'), True),    # CPU fault chain: offset
    (re.compile(r'\bexe 0x([0-9a-f]+)'), False),           # STACKS dump: address
)


def find_symbolizer():
    found = shutil.which('llvm-symbolizer')
    if found:
        return found
    root = os.environ.get('MSYS2_ROOT', 'C:/msys64')
    for candidate in (Path(root) / 'clang64/bin/llvm-symbolizer.exe', Path('C:/msys64/clang64/bin/llvm-symbolizer.exe')):
        if candidate.is_file():
            return str(candidate)
    return None


def addresses(text):
    out = set()
    for pattern, is_offset in PATTERNS:
        for m in pattern.finditer(text):
            value = int(m.group(1), 16)
            out.add(IMAGE_BASE + value if is_offset else value)
    return sorted(a for a in out if IMAGE_BASE < a < IMAGE_BASE + 0x10000000)


def symbolize(log, exe):
    tool = find_symbolizer()
    if not tool or not Path(exe).is_file():
        return None
    text = Path(log).read_text(encoding='utf-8', errors='replace')
    addrs = addresses(text)
    if not addrs:
        return None
    result = subprocess.run([tool, f'--obj={exe}', '--functions=short', '--inlines'] + [hex(a) for a in addrs],
                            capture_output=True, text=True, timeout=120)
    blocks = result.stdout.strip().split('\n\n')
    lines = []
    for address, block in zip(addrs, blocks):
        parts = block.split('\n')
        frames = []
        for i in range(0, len(parts) - 1, 2):
            location = parts[i + 1].replace('\\', '/')
            location = location.split('/gpu/', 1)[-1] if '/gpu/' in location else location.split('/src/', 1)[-1]
            frames.append(f'{parts[i]} {location}')
        offset = address - IMAGE_BASE
        lines.append(f'  exe+{offset:#x}  ' + ('  <- inlined in  '.join(frames) if frames else '??'))
    return lines


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(__doc__)
        return 2
    log = args[0]
    exe = args[1] if len(args) > 1 else str(Path(__file__).resolve().parent.parent / 'out/gpu/bb-probe.exe')
    lines = symbolize(log, exe)
    if lines is None:
        return 1
    if '--append' in sys.argv:
        stale = Path(exe).stat().st_mtime > Path(log).stat().st_mtime
        with open(log, 'a', encoding='utf-8') as f:
            f.write(f'\nSYMBOLS (llvm-symbolizer, {Path(exe).name}'
                    f'{"; exe rebuilt after this log, lines may be off" if stale else ""}):\n')
            f.write('\n'.join(lines) + '\n')
    else:
        print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
