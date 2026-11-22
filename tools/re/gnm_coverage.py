#!/usr/bin/env python3
"""Static coverage audit of the native GNM cutover (read-only).

Lists the eboot PM4 emitters (functions in the Ghidra decompile exports out/re/decomp/0085..0088
that contain PM4 header or NOP payload constants), with their callers from the "called by" header
line, and the sceGnm Table entries of gpu/native/gnm/gnm_common.h. It reports which emitters have a
detour row in gpu/sotc/hook_gnm_wNN.inc and which Table entries a RegisterWNN body sets.

Heuristics (state them when quoting numbers):
  - A PM4 header constant is a 32-bit literal whose top two bits are 11 (type 3) and whose opcode
    byte (bits 8-15) is an opcode of gpu/shadps4/video_core/amdgpu/pm4_opcodes.h. A NOP payload is a
    literal 0x6875xxxx (PM4CmdNop::PayloadType). Other literals are ignored, so emitters that only
    write a header through a variable are missed.
  - "called by" comes from the decompile header and lists the direct callers only.

Usage: python -I tools/re/gnm_coverage.py [--all]
  default: summary plus the emitters without a detour row
  --all:   also list every emitter and every Table entry
Read-only: it writes nothing.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DECOMP_DIRS = ["0085", "0086", "0087", "0088"]
PM4_OPCODES_H = ROOT / "gpu/shadps4/video_core/amdgpu/pm4_opcodes.h"
NATIVE_GNM_DIR = ROOT / "gpu/native/gnm"
GNM_COMMON_H = NATIVE_GNM_DIR / "gnm_common.h"
HOOK_INC_GLOB = "gpu/sotc/hook_gnm_w*.inc"
NAMES_JSON = ROOT / "out/re/orbis/names_orbis.json"
GNMDRIVER_CPP = ROOT / "gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp"

HEADER_RE = re.compile(r"^// (\w+) @ 0x([0-9a-fA-F]+) size (\d+)(?:, called by: (.*))?$")
LITERAL_RE = re.compile(r"0x([0-9a-fA-F]{8})\b")
ROW_RE = re.compile(r"\{\s*0x([0-9a-fA-F]+)\s*,\s*\(void\*\)&GnmW(\d+)_")
SET_RE = re.compile(r"\bt\.(\w+)\s*=")
TABLE_MEMBER_RE = re.compile(r"bool\s*\(\*(\w+)\)\s*\(")


def load_pm4_opcodes():
    """Opcode values of the PM4ItOpcode enum (type-3 opcode byte)."""
    opcodes = set()
    if not PM4_OPCODES_H.exists():
        return opcodes
    for m in re.finditer(r"^\s*\w+\s*=\s*0x([0-9a-fA-F]+)\s*,", PM4_OPCODES_H.read_text(encoding="utf-8", errors="replace"), re.M):
        opcodes.add(int(m.group(1), 16))
    return opcodes


def pm4_constants(text, opcodes):
    """PM4 header and NOP payload literals found in a decompile body."""
    found = []
    for m in LITERAL_RE.finditer(text):
        value = int(m.group(1), 16)
        if (value >> 30) == 0x3 and ((value >> 8) & 0xFF) in opcodes:
            found.append(f"hdr:0x{value:08x}")
        elif (value & 0xFFFF0000) == 0x68750000:
            found.append(f"payload:0x{value:08x}")
    return sorted(set(found))


def scan_emitters(opcodes):
    emitters = []
    for sub in DECOMP_DIRS:
        for path in sorted((ROOT / "out/re/decomp" / sub).glob("*.c")):
            text = path.read_text(encoding="utf-8", errors="replace")
            first, _, body = text.partition("\n")
            m = HEADER_RE.match(first.strip())
            if not m:
                continue
            constants = pm4_constants(body, opcodes)
            if not constants:
                continue
            callers = (m.group(4) or "").split()
            emitters.append({
                "name": m.group(1),
                "va": int(m.group(2), 16),
                "size": int(m.group(3)),
                "callers": callers,
                "constants": constants,
                "file": str(path.relative_to(ROOT)).replace("\\", "/"),
            })
    emitters.sort(key=lambda e: e["va"])
    return emitters


def scan_hook_rows():
    """VA -> worker number for every detour row in the hook_gnm_wNN.inc files."""
    rows = {}
    for path in sorted(ROOT.glob(HOOK_INC_GLOB)):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            stripped = line.strip()
            # Comment lines of the .inc files quote an example row; only real rows count.
            if stripped.startswith("//") or stripped.startswith("/*") or not stripped.startswith("{"):
                continue
            m = ROW_RE.search(stripped)
            if m:
                rows[int(m.group(1), 16)] = int(m.group(2))
    return rows


def scan_table_members():
    """Member names of struct Table in gnm_common.h, in declaration order."""
    text = GNM_COMMON_H.read_text(encoding="utf-8", errors="replace")
    start = text.find("struct Table {")
    end = text.find("\n};", start)
    body = text[start:end] if start >= 0 and end > start else ""
    names = []
    for m in TABLE_MEMBER_RE.finditer(body):
        names.append(m.group(1))
    return names


def scan_register_sets():
    """Table member -> worker number, from the t.<Member> = ... lines of the RegisterWNN bodies."""
    sets = {}
    for path in sorted(NATIVE_GNM_DIR.glob("w[0-9][0-9]_*.cpp")):
        worker = int(path.name[1:3])
        for m in SET_RE.finditer(path.read_text(encoding="utf-8", errors="replace")):
            sets.setdefault(m.group(1), worker)
    return sets


def scan_gnmdriver_calls():
    """Table member -> number of sceGnm* driver call sites that read GetTable().<member>."""
    counts = {}
    if not GNMDRIVER_CPP.exists():
        return counts
    for m in re.finditer(r"GetTable\(\)\.(\w+)", GNMDRIVER_CPP.read_text(encoding="utf-8", errors="replace")):
        counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    return counts


def scan_orbis_gnm_imports():
    """sceGnm* import symbols named by the Orbis name map (empty when the map is missing)."""
    if not NAMES_JSON.exists():
        return set()
    import json
    data = json.loads(NAMES_JSON.read_text(encoding="utf-8"))
    names = set()
    for entry in data:
        symbol = entry.get("symbol", "")
        if symbol.startswith("sceGnm"):
            names.add(symbol)
    return names


def main(argv):
    show_all = "--all" in argv
    opcodes = load_pm4_opcodes()
    emitters = scan_emitters(opcodes)
    rows = scan_hook_rows()
    members = scan_table_members()
    sets = scan_register_sets()
    calls = scan_gnmdriver_calls()
    imports = scan_orbis_gnm_imports()

    replaced = [e for e in emitters if e["va"] in rows]
    unreplaced = [e for e in emitters if e["va"] not in rows]
    per_worker = {}
    for e in replaced:
        w = rows[e["va"]]
        per_worker[w] = per_worker.get(w, 0) + 1

    print("== PM4 emitters (decompile 0085-0088, PM4 literal found) ==")
    print(f"emitters: {len(emitters)}  with detour row: {len(replaced)}  without: {len(unreplaced)}")
    for w in sorted(per_worker):
        print(f"  W{w:02d} detour rows matched: {per_worker[w]}")
    print(f"  rows in hook_gnm_w*.inc (any VA): {len(rows)}")

    print()
    print("== sceGnm Table entries (gnm_common.h) ==")
    set_members = [m for m in members if m in sets]
    print(f"entries: {len(members)}  set by a RegisterWNN body: {len(set_members)}")
    per_table_worker = {}
    for m in set_members:
        per_table_worker[sets[m]] = per_table_worker.get(sets[m], 0) + 1
    for w in sorted(per_table_worker):
        print(f"  W{w:02d} entries set: {per_table_worker[w]}")
    if imports:
        mapped = [m for m in members if ("sceGnm" + m) in imports]
        print(f"Orbis sceGnm import symbols: {len(imports)}; Table members matching an import: {len(mapped)}")

    print()
    print("== Table members not set by any worker (PM4 path only) ==")
    for m in members:
        if m not in sets:
            print(f"  {m}  (gnmdriver GetTable() call sites: {calls.get(m, 0)})")

    print()
    print("== Emitters without a detour row (first 40 by VA unless --all) ==")
    limit = None if show_all else 40
    for e in unreplaced[:limit]:
        callers = ",".join(e["callers"][:4]) if e["callers"] else "-"
        print(f"  0x{e['va']:08x} {e['name']} size={e['size']} callers=[{callers}] "
              f"pm4={' '.join(e['constants'][:3])}")
    if limit is not None and len(unreplaced) > limit:
        print(f"  ... {len(unreplaced) - limit} more (use --all)")

    if show_all:
        print()
        print("== Emitters with a detour row ==")
        for e in replaced:
            print(f"  0x{e['va']:08x} {e['name']} W{rows[e['va']]:02d}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
