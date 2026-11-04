#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Classify the vertex programs of SOTC_PRETRANSLATE packs by SPIR-V similarity (RE, read-only).

Input: sotc_pretranslate.pack files (format in gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp,
PrePackAdd / PrePackFinish): "SOTCPRE1", records (u64 hash, u32 perm, u32 meta bytes, u32 spv bytes, meta, spv),
an index (u32 count; u64 hash, u32 perm, u64 offset, u32 meta bytes, u32 spv bytes), u64 index offset, "SOTCPRE1".

Vertex modules are picked by the OpEntryPoint execution model (0 = Vertex), not by the predict table. The
predict table (user/shader_predict/predict_table.tsv: hash, footer type, pass name, file count) gives the pass name;
footer type 1 must agree with the execution model, and the script reports any disagreement.

Each module is disassembled with spirv-dis (SPIRV-Tools, --raw-id), then normalized:
  - header comments and debug instructions (OpName, OpMemberName, OpString, OpSource*, OpLine,
    OpModuleProcessed) are dropped;
  - %ids are renumbered in order of first appearance.
Keys:
  exact      the normalized text (identical modules have equal keys);
  shape      the normalized text with every numeric literal replaced by '#' (same instructions, other constants);
  fetch      the normalized fetch lines: Input and buffer variables (OpVariable, their pointer types, their
             decorations; a fetch-id in OpEntryPoint's interface is dropped from the transform text);
  transform  the normalized text without the fetch lines.
Fetch/transform split is by declaration only: the decode arithmetic that consumes a buffer load stays in the
transform text, so the fetch share reported is a lower bound.

Categories per module (first matching rule):
  identical        exact key shared with another module
  constants        shape shared with another module, exact key not
  fetch-only       neither; transform key shared (the only difference is fetch lines)
  transform-only   neither; fetch key shared
  both             none of the above

Usage (repo root, any Python 3 stdlib):
  python tools/re/vertex_dupes.py --packs user/cache/CUSA08809_v13272bcc/sotc_pretranslate.pack \
      --packs user/cache/CUSA08809_v8dae828b/sotc_pretranslate.pack --table user/shader_predict/predict_table.tsv \
      [--spirv-dis C:/msys64/clang64/bin/spirv-dis.exe] [--tsv OUT.tsv] [--json OUT.json] [--workers 8]
Prints a summary to stdout. Does not write into the repository unless --tsv/--json point there.
"""

import argparse
import collections
import concurrent.futures
import difflib
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

MAGIC = b"SOTCPRE1"
SPV_MAGIC = 0x07230203
OP_ENTRY_POINT = 15
OP_FUNCTION = 54
VERTEX_MODEL = 0
DEBUG_OPS = {"OpName", "OpMemberName", "OpString", "OpSource", "OpSourceContinued", "OpSourceExtension",
             "OpLine", "OpModuleProcessed", "OpNoLine"}
FETCH_SC = {"Input", "StorageBuffer", "Uniform", "UniformConstant"}
BUFFER_SC = {"StorageBuffer", "Uniform", "UniformConstant"}
ID_RE = re.compile(r"%\d+")
NUM_RE = re.compile(r"^-?(\d+(\.\d*)?([eE][-+]?\d+)?|0x[0-9a-fA-F]+|inf|nan)$")
DEFAULT_DIS = Path(os.environ.get("MSYS2_ROOT", "C:/msys64")) / "clang64/bin/spirv-dis.exe"


def entry_model(spv):
    """Execution model of the first OpEntryPoint, or None when the module has none before its functions."""
    if len(spv) < 20 or struct.unpack_from("<I", spv, 0)[0] != SPV_MAGIC:
        return None
    i = 20
    n = len(spv)
    while i + 4 <= n:
        w = struct.unpack_from("<I", spv, i)[0]
        wc, op = w >> 16, w & 0xFFFF
        if wc == 0:
            return None
        if op == OP_ENTRY_POINT:
            return struct.unpack_from("<I", spv, i + 4)[0]
        if op == OP_FUNCTION:
            return None
        i += wc * 4
    return None


def read_pack(path):
    """Yield (hash, perm, spv bytes) for every record of one pack, in file order."""
    with open(path, "rb") as f:
        f.seek(-16, os.SEEK_END)
        index_off = struct.unpack("<Q", f.read(8))[0]
        if f.read(8) != MAGIC:
            raise SystemExit(f"{path}: no SOTCPRE1 footer")
        f.seek(index_off)
        count = struct.unpack("<I", f.read(4))[0]
        recs = [struct.unpack("<QIQII", f.read(28)) for _ in range(count)]
        for h, perm, off, meta_sz, spv_sz in sorted(recs, key=lambda r: r[2]):
            f.seek(off + meta_sz)
            yield h, perm, f.read(spv_sz)


def load_table(path):
    """hash -> (footer type, pass name) from predict_table.tsv (first row wins)."""
    table = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 3:
                continue
            try:
                h = int(cols[0], 16)
            except ValueError:
                continue
            table.setdefault(h, (int(cols[1]), cols[2]))
    return table


def disassemble(spv, dis):
    proc = subprocess.run([dis, "--raw-id", "--no-indent", "--no-header", "--no-color", "-"],
                          input=spv, capture_output=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode("utf-8", "replace")[:300])
    return proc.stdout.decode("utf-8", "replace").splitlines()


def op_of(line):
    m = re.match(r"%\d+ = (\S+)", line)
    return m.group(1) if m else line.split(" ", 1)[0]


def renumber(lines):
    """Renumber %ids in order of first appearance. Returns the new lines (one per input line)."""
    mapping = {}

    def rep(m):
        k = m.group(0)
        if k not in mapping:
            mapping[k] = "%" + str(len(mapping) + 1)
        return mapping[k]

    return [ID_RE.sub(rep, s) for s in lines]


def mask(line):
    return " ".join("#" if (not t.startswith("%") and NUM_RE.match(t)) else t for t in line.split())


def sha(lines):
    return hashlib.sha1("\n".join(lines).encode("utf-8")).hexdigest()[:16]


def analyze(text_lines):
    kept = []
    for s in text_lines:
        s = s.strip()
        if not s or s.startswith(";"):
            continue
        if op_of(s) in DEBUG_OPS:
            continue
        kept.append(s)

    fvars = {}   # id -> storage class, for OpVariable of fetch storage classes
    fptr = set() # pointer types of those storage classes
    for s in kept:
        m = re.match(r"(%\d+) = OpVariable (%\d+) (\w+)", s)
        if m and m.group(3) in FETCH_SC:
            fvars[m.group(1)] = m.group(3)
            continue
        m = re.match(r"(%\d+) = OpTypePointer (\w+) ", s)
        if m and m.group(2) in FETCH_SC:
            fptr.add(m.group(1))
    fset = set(fvars) | fptr

    flags = []
    for s in kept:
        f = False
        m = re.match(r"(%\d+) = (OpVariable|OpTypePointer) ", s)
        if m and m.group(1) in fset:
            f = True
        elif op_of(s) in ("OpDecorate", "OpMemberDecorate"):
            toks = s.split()
            f = len(toks) > 1 and toks[1] in fset
        flags.append(f)

    transform_lines = []
    for s, f in zip(kept, flags):
        if f:
            continue
        if op_of(s) == "OpEntryPoint":
            s = " ".join(t for t in s.split() if t not in fvars)
        transform_lines.append(s)
    fetch_lines = [s for s, f in zip(kept, flags) if f]

    full = renumber(kept)
    trans = renumber(transform_lines)
    fetch = renumber(fetch_lines)

    access = 0
    for s in kept:
        toks = s.split()
        if len(toks) > 4 and toks[2] in ("OpAccessChain", "OpImageRead", "OpImageFetch", "OpImageWrite"):
            if toks[4] in fvars and fvars[toks[4]] in BUFFER_SC:
                access += 1

    return {
        "full": full,
        "flags": flags,
        "exact": sha(full),
        "shape": sha([mask(s) for s in full]),
        "transform": sha(trans),
        "fetch": sha(fetch),
        "n_instr": len(kept),
        "n_fetch": len(fetch_lines),
        "n_inputs": sum(1 for v in fvars.values() if v == "Input"),
        "n_bufvars": sum(1 for v in fvars.values() if v in BUFFER_SC),
        "n_access": access,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", action="append", required=True)
    ap.add_argument("--table", default="user/shader_predict/predict_table.tsv")
    ap.add_argument("--spirv-dis", default=str(DEFAULT_DIS))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--tsv")
    ap.add_argument("--json")
    args = ap.parse_args()

    table = load_table(args.table)

    # 1. Collect vertex modules per pack, keyed by (hash, perm). Every distinct SPIR-V blob is analyzed.
    per_pack = []     # [{(hash, perm): blob sha}] in --packs order
    blobs = {}        # spv sha -> spv bytes
    pack_stats = []
    for pack in args.packs:
        n_rec = n_vert = n_type1_not_vert = n_vert_not_type1 = 0
        found = {}
        for h, perm, spv in read_pack(pack):
            n_rec += 1
            model = entry_model(spv)
            footer = table.get(h, (None, None))[0]
            if model == VERTEX_MODEL:
                n_vert += 1
                if footer != 1:
                    n_vert_not_type1 += 1
            elif footer == 1:
                n_type1_not_vert += 1
            if model != VERTEX_MODEL or (h, perm) in found:
                continue
            digest = hashlib.sha1(spv).hexdigest()
            found[(h, perm)] = digest
            blobs.setdefault(digest, spv)
        per_pack.append(found)
        pack_stats.append({"pack": pack, "records": n_rec, "vertex": n_vert,
                           "vertex_not_footer1": n_vert_not_type1, "footer1_not_vertex": n_type1_not_vert})
        print(f"[pack] {pack}: {n_rec} records, {n_vert} vertex, "
              f"vertex-not-table-type1 {n_vert_not_type1}, table-type1-not-vertex {n_type1_not_vert}")
    modules = {}      # (hash, perm) -> blob sha, first pack wins
    for found in per_pack:
        for key, digest in found.items():
            modules.setdefault(key, digest)
    print(f"[modules] {len(modules)} unique (hash, perm) vertex modules, {len(blobs)} distinct SPIR-V blobs")

    # 2. Disassemble each distinct blob once and analyze it.
    analyses = {}
    errors = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {pool.submit(disassemble, spv, args.spirv_dis): digest for digest, spv in blobs.items()}
        for fut in concurrent.futures.as_completed(futs):
            digest = futs[fut]
            try:
                analyses[digest] = analyze(fut.result())
            except Exception as exc:  # report and continue
                errors += 1
                print(f"[error] {digest}: {exc}", file=sys.stderr)
    blobs.clear()
    # Cross-pack agreement. The perm index is assigned in the order variants are first used, so it can differ
    # between runs: compare the multiset of exact keys per program hash instead of (hash, perm).
    if len(per_pack) > 1:
        for i in range(1, len(per_pack)):
            def multisets(found):
                out = collections.defaultdict(list)
                for (h, _perm), digest in found.items():
                    if digest in analyses:
                        out[h].append(analyses[digest]["exact"])
                return {h: sorted(v) for h, v in out.items()}
            ms_a, ms_b = multisets(per_pack[0]), multisets(per_pack[i])
            common = set(ms_a) & set(ms_b)
            same = sum(1 for h in common if ms_a[h] == ms_b[h])
            raw = sum(1 for k in set(per_pack[0]) & set(per_pack[i]) if per_pack[0][k] == per_pack[i][k])
            print(f"[agreement] pack 1 vs pack {i + 1}: {len(common)} program hashes in both; {same} with the same "
                  f"multiset of normalized modules; {len(set(per_pack[0]) & set(per_pack[i]))} common (hash, perm), "
                  f"{raw} with equal SPIR-V bytes")
    print(f"[analysis] {len(analyses)} blobs analyzed, {errors} failed")

    # 3. One record per module.
    mods = []
    for (h, perm), digest in sorted(modules.items()):
        a = analyses.get(digest)
        if a is None:
            continue
        mods.append({"hash": h, "perm": perm, "pass": table.get(h, (None, "?"))[1], "blob": digest, **a})

    exact_n = collections.Counter(m["exact"] for m in mods)
    tkey_n = collections.Counter(m["transform"] for m in mods)
    fkey_n = collections.Counter(m["fetch"] for m in mods)
    shape_exacts = collections.defaultdict(set)
    shape_mods = collections.Counter()
    for m in mods:
        shape_exacts[m["shape"]].add(m["exact"])
        shape_mods[m["shape"]] += 1

    # Representative full text per exact key (for the constants analysis).
    rep = {}
    for m in mods:
        rep.setdefault(m["exact"], m)

    for m in mods:
        if exact_n[m["exact"]] > 1:
            cat = "identical"
        elif len(shape_exacts[m["shape"]]) > 1:
            cat = "constants"
        elif tkey_n[m["transform"]] > 1:
            cat = "fetch-only"
        elif fkey_n[m["fetch"]] > 1:
            cat = "transform-only"
        else:
            cat = "both"
        m["cat"] = cat

    # 4. Constants: which lines differ inside shape groups with more than one exact key.
    slot_ops = collections.Counter()
    slot_samples = collections.defaultdict(list)
    const_groups = []
    for shape, exacts in shape_exacts.items():
        if len(exacts) < 2:
            continue
        reps = [rep[e] for e in exacts]
        ref = reps[0]
        n_lines = len(ref["full"])
        varying = []
        for i in range(n_lines):
            vals = {r["full"][i] for r in reps}
            if len(vals) > 1:
                varying.append(i)
        n_fetch_slots = sum(1 for i in varying if ref["flags"][i])
        ops = collections.Counter()
        for i in varying:
            vals = sorted({r["full"][i] for r in reps})
            op = op_of(ref["full"][i])
            if op in ("OpDecorate", "OpMemberDecorate"):
                op = op + " " + ref["full"][i].split()[2 if op == "OpDecorate" else 3]
            ops[op] += 1
            slot_ops[op] += 1
            if len(slot_samples[op]) < 6:
                slot_samples[op].append(" | ".join(v[:90] for v in vals[:3]))
        const_groups.append({"shape": shape, "exact_variants": len(exacts), "modules": shape_mods[shape],
                             "varying_slots": len(varying), "varying_fetch_slots": n_fetch_slots,
                             "passes": collections.Counter(r["pass"] for r in reps),
                             "ops": ops, "instr": ref["n_instr"]})

    # 4b. Transform differences: exact variants that share the fetch key (only the transform text differs).
    #     difflib on the opcode sequence with %ids dropped (a positional line diff is swamped by renumbering).
    #     OpConstant lines keep their value. Inserted/deleted entries are counted by key, split into declarations
    #     and body.
    by_fetch = collections.defaultdict(dict)
    for m in mods:
        by_fetch[m["fetch"]].setdefault(m["exact"], m)
    decl_ops = ("OpType", "OpConstant", "OpVariable", "OpDecorate", "OpMemberDecorate", "OpExecutionMode",
                "OpCapability", "OpExtension", "OpExtInstImport", "OpEntryPoint", "OpMemoryModel")

    def key(s):
        op = op_of(s)
        return s.split()[-1] if op == "OpConstant" else op

    body_ops = collections.Counter()
    decl_only_ops = collections.Counter()
    pairs = 0
    same_multiset = 0
    access_delta = collections.Counter()
    for fk, byexact in sorted(by_fetch.items(), key=lambda kv: -len(kv[1])):
        if len(byexact) < 2 or pairs >= 300:
            continue
        pairs += 1
        a, b = list(byexact.values())[:2]
        sa = [key(s) for s in a["full"]]
        sb = [key(s) for s in b["full"]]
        if collections.Counter(sa) == collections.Counter(sb):
            same_multiset += 1
        access_delta[(a["n_access"] - b["n_access"]) > 0] += 1
        sm = difflib.SequenceMatcher(None, sa, sb, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            for k in sa[i1:i2] + sb[j1:j2]:
                if k.startswith(decl_ops):
                    decl_only_ops[k.split(" ")[0]] += 1
                else:
                    body_ops[k] += 1
    print(f"\n[transform differences] {pairs} fetch groups with >1 exact variant (first two compared); "
          f"{same_multiset} pairs have the same opcode multiset (reordering or renumbering only)")
    print("  body entries on differing opcode runs: " + ", ".join(f"{k} {v}" for k, v in body_ops.most_common(20)))
    print("  declaration entries on differing runs: " + ", ".join(f"{k} {v}" for k, v in decl_only_ops.most_common(12)))

    # 5. Report.
    tot_instr = sum(m["n_instr"] for m in mods)
    print(f"\n[categories] {len(mods)} modules, {tot_instr} instructions (debug dropped)")
    cats = ["identical", "constants", "fetch-only", "transform-only", "both"]
    cnt = collections.Counter(m["cat"] for m in mods)
    ins = collections.Counter()
    for m in mods:
        ins[m["cat"]] += m["n_instr"]
    for c in cats:
        print(f"  {c:15s} modules {cnt[c]:6d} ({100*cnt[c]/len(mods):5.1f}%)  instr {ins[c]:10d} "
              f"({100*ins[c]/max(1,tot_instr):5.1f}%)")
    removable = sum(m["n_instr"] for m in mods if m["cat"] == "identical") - \
        sum(rep[e]["n_instr"] for e in exact_n if exact_n[e] > 1)
    print(f"  identical copies removable by merging: {removable} instr "
          f"({100*removable/max(1,tot_instr):.1f}% of all), modules "
          f"{sum(c - 1 for e, c in exact_n.items() if c > 1)}")
    print(f"[keys] distinct exact {len(exact_n)}, distinct shapes {len(shape_exacts)}, "
          f"shapes with >1 exact {len(const_groups)}, distinct transform {len(tkey_n)}, distinct fetch {len(fkey_n)}")
    fetch_instr = sum(m["n_fetch"] for m in mods)
    print(f"[fetch] fetch lines {fetch_instr} of {tot_instr} instr ({100*fetch_instr/max(1,tot_instr):.1f}%); "
          f"Input vars per module avg {sum(m['n_inputs'] for m in mods)/max(1,len(mods)):.2f}, "
          f"buffer vars avg {sum(m['n_bufvars'] for m in mods)/max(1,len(mods)):.2f}, "
          f"buffer accesses avg {sum(m['n_access'] for m in mods)/max(1,len(mods)):.2f}")

    print("\n[per pass] pass modules exact shapes identical constants fetch-only transform-only both instr_removable%")
    per_pass = collections.defaultdict(list)
    for m in mods:
        per_pass[m["pass"]].append(m)
    rows = []
    for p, ms in per_pass.items():
        c = collections.Counter(m["cat"] for m in ms)
        ex = len({m["exact"] for m in ms})
        sh = len({m["shape"] for m in ms})
        ti = sum(m["n_instr"] for m in ms)
        rem = sum(m["n_instr"] for m in ms if m["cat"] == "identical") - \
            sum(rep[e]["n_instr"] for e in {m["exact"] for m in ms if exact_n[m["exact"]] > 1})
        rows.append((len(ms), p, ex, sh, c, 100 * rem / max(1, ti)))
    for n, p, ex, sh, c, rp in sorted(rows, reverse=True):
        print(f"  {p:32s} {n:6d} {ex:6d} {sh:6d} {c['identical']:6d} {c['constants']:6d} "
              f"{c['fetch-only']:6d} {c['transform-only']:6d} {c['both']:6d}   {rp:5.1f}")

    print("\n[biggest exact groups] size, instr, passes, hashes (perm)")
    ebig = collections.defaultdict(list)
    for m in mods:
        ebig[m["exact"]].append(m)
    for e, ms in sorted(ebig.items(), key=lambda kv: -len(kv[1]))[:15]:
        pc = collections.Counter(x["pass"] for x in ms)
        sample = ", ".join(f"{x['hash']:016x}({x['perm']})" for x in ms[:3])
        print(f"  {len(ms):4d} {ms[0]['n_instr']:6d} {dict(pc.most_common(3))}  {sample}")

    print("\n[biggest shape groups] modules, exact variants, passes")
    for s, n in shape_mods.most_common(12):
        pc = collections.Counter(m["pass"] for m in mods if m["shape"] == s)
        print(f"  {n:4d} {len(shape_exacts[s]):4d} {dict(pc.most_common(3))}")

    print("\n[constants-only groups] shape groups with >1 exact variant")
    print(f"  groups {len(const_groups)}; varying slots total {sum(g['varying_slots'] for g in const_groups)}, "
          f"of which in fetch lines {sum(g['varying_fetch_slots'] for g in const_groups)}")
    print("  varying slots by opcode (count, samples of differing values):")
    for op, n in slot_ops.most_common(25):
        print(f"    {op:36s} {n:6d}  e.g. {slot_samples[op][0] if slot_samples[op] else ''}")

    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8") as f:
            f.write("hash\tperm\tpass\tcat\tn_instr\tn_fetch\tn_inputs\tn_bufvars\tn_access\texact\tshape\ttransform\tfetch\n")
            for m in mods:
                f.write("\t".join(str(x) for x in [f"{m['hash']:016x}", m["perm"], m["pass"], m["cat"], m["n_instr"],
                                                   m["n_fetch"], m["n_inputs"], m["n_bufvars"], m["n_access"],
                                                   m["exact"], m["shape"], m["transform"], m["fetch"]]) + "\n")
        print(f"\n[tsv] {args.tsv}")
    if args.json:
        out = {"packs": pack_stats, "modules": len(mods), "categories": cnt, "instr": ins,
               "const_groups": [{**g, "passes": dict(g["passes"]), "ops": dict(g["ops"])} for g in const_groups],
               "slot_ops": slot_ops, "slot_samples": slot_samples}
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1, default=dict)
        print(f"[json] {args.json}")


if __name__ == "__main__":
    main()
