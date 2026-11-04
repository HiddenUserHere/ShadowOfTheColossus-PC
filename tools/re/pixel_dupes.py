#!/usr/bin/env python3
"""Pixel-shader duplicate analysis over the port's SPIR-V pre-pack (read-only).

Input: <cache>/sotc_pretranslate.pack per shader cache folder (default: the two cold runs
user/cache/CUSA08809_v13272bcc and CUSA08809_v8dae828b) and user/shader_predict/predict_table.tsv.

Pack format (gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp, PrePackAdd/PrePackFinish):
  header per variant: u64 hash, u32 perm, u32 meta_size, u32 spv_size, then meta bytes and SPIR-V words;
  trailer: u64 index_offset, magic "SOTCPRE1" at EOF; index at index_offset: u32 count, then per entry
  u64 hash, u32 perm, u64 offset (of meta), u32 meta_size, u32 spv_size.

Stage: OpEntryPoint execution model (4 = Fragment). Only Fragment modules are analysed.

Shape levels (after dropping debug instructions and renumbering ids by first appearance):
  E  exact text
  C  E with OpConstant/OpSpecConstant literal values masked (same shape, constants may differ)
  CD C with OpDecorate/OpMemberDecorate trailing literal (Binding, Location, Offset, ...) masked
  S  opcode sequence only (structure without operands)

Disassembly: spirv-dis (MSYS2 CLANG64), cached per SHA-1 in --work.

Usage:
  python tools/re/pixel_dupes.py [--cache DIR ...] [--work DIR] [--out DIR] [--jobs 8]
Writes DIR/pixel_variants_<folder>.tsv (one row per variant) and DIR/pixel_summary.json, prints a summary.
"""

import argparse
import collections
import concurrent.futures as cf
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CACHES = [REPO / "user/cache/CUSA08809_v13272bcc", REPO / "user/cache/CUSA08809_v8dae828b"]
PREDICT = REPO / "user/shader_predict/predict_table.tsv"
PACK_NAME = "sotc_pretranslate.pack"
PACK_MAGIC = b"SOTCPRE1"
FRAGMENT = 4
OP_ENTRY_POINT = 15
OP_FUNCTION = 54

DEBUG_OPS = {
    "OpSource", "OpSourceContinued", "OpSourceExtension", "OpName", "OpMemberName",
    "OpString", "OpLine", "OpNoLine", "OpModuleProcessed",
}
CONST_OPS = {"OpConstant", "OpSpecConstant"}
DECOR_OPS = {"OpDecorate", "OpMemberDecorate"}
TEX_PREFIXES = ("OpImageSample", "OpImageFetch", "OpImageGather", "OpImageRead", "OpImageDrefGather",
                "OpImageQuery", "OpImageSparse")
KILL_OPS = {"OpKill", "OpDemoteToHelperInvocation", "OpDemoteToHelperInvocationEXT"}
BRANCH_OPS = {"OpBranchConditional", "OpSwitch", "OpLoopMerge"}
DERIV_OPS = {"OpDPdx", "OpDPdy", "OpFwidth", "OpDPdxFine", "OpDPdyFine", "OpFwidthFine",
             "OpDPdxCoarse", "OpDPdyCoarse", "OpFwidthCoarse"}

ID_RE = re.compile(r"%(\d+)")
RESULT_RE = re.compile(r"^%(\d+)\s*=\s*(\S+)\s*(.*)$")
PLAIN_RE = re.compile(r"^(\S+)\s*(.*)$")


def find_spirv_dis():
    found = shutil.which("spirv-dis")
    if found:
        return found
    for cand in ("C:/msys64/clang64/bin/spirv-dis.exe", "/clang64/bin/spirv-dis.exe", "/i/msys64/clang64/bin/spirv-dis.exe"):
        if Path(cand).exists():
            return cand
    sys.exit("spirv-dis not found (MSYS2 CLANG64 spirv-tools)")


def read_pack_index(path):
    """Return (open file, entries); the caller closes the file."""
    f = open(path, "rb")
    f.seek(-16, os.SEEK_END)
    (index_offset,) = struct.unpack("<Q", f.read(8))
    if f.read(8) != PACK_MAGIC:
        f.close()
        raise SystemExit(f"{path}: bad trailer magic")
    f.seek(index_offset)
    (count,) = struct.unpack("<I", f.read(4))
    entries = [struct.unpack("<QIQII", f.read(28)) for _ in range(count)]
    return f, entries


def exec_model(words):
    i = 5
    n = len(words)
    while i < n:
        wc = words[i] >> 16
        op = words[i] & 0xFFFF
        if wc == 0:
            break
        if op == OP_ENTRY_POINT:
            return words[i + 1]
        if op == OP_FUNCTION:
            break
        i += wc
    return None


def disasm(spirv_dis, spv, sha, work):
    cached = work / f"{sha}.dis"
    if cached.exists():
        return cached.read_text(encoding="utf-8")
    tmp = work / f"{sha}.spv"
    tmp.write_bytes(spv)
    try:
        res = subprocess.run([spirv_dis, "--no-header", "--raw-id", "--no-color", str(tmp)],
                             capture_output=True)
    finally:
        tmp.unlink(missing_ok=True)
    if res.returncode != 0:
        raise RuntimeError(f"spirv-dis failed for {sha}: {res.stderr.decode('utf-8', 'replace')[:200]}")
    text = res.stdout.decode("utf-8", "replace")
    cached.write_text(text, encoding="utf-8")
    return text


def normalize(dis_text, level):
    """Return normalized text for level in E, C, CD, S. Ids renumbered by first appearance."""
    out_lines = []
    for raw in dis_text.splitlines():
        s = raw.strip()
        if not s or s.startswith(";"):
            continue
        m = RESULT_RE.match(s)
        if m:
            res_id, opname, rest = m.group(1), m.group(2), m.group(3)
            prefix = f"%{res_id} = "
        else:
            m2 = PLAIN_RE.match(s)
            if not m2:
                continue
            opname, rest = m2.group(1), m2.group(2)
            prefix = ""
        if opname in DEBUG_OPS:
            continue
        if level == "S":
            out_lines.append(opname)
            continue
        if opname in CONST_OPS and level in ("C", "CD"):
            parts = rest.split()
            if parts:
                rest = " ".join([parts[0], "#"]) if len(parts) > 1 else parts[0]
        elif opname in DECOR_OPS and level == "CD":
            parts = rest.split()
            if parts and re.fullmatch(r"-?\d+(\.\d+)?", parts[-1]):
                parts[-1] = "#"
                rest = " ".join(parts)
        out_lines.append(f"{prefix}{opname} {rest}".rstrip())
    if level == "S":
        return "\n".join(out_lines)
    mapping = {}

    def repl(mo):
        key = mo.group(1)
        if key not in mapping:
            mapping[key] = len(mapping)
        return f"%{mapping[key]}"

    return "\n".join(ID_RE.sub(repl, line) for line in out_lines)


def opnames_of(norm_e):
    names = []
    for line in norm_e.splitlines():
        m = RESULT_RE.match(line)
        if m:
            names.append(m.group(2))
        else:
            names.append(line.split(None, 1)[0] if line else "")
    return names


def bucket(n):
    if n == 0:
        return "0"
    if n <= 2:
        return "1-2"
    if n <= 4:
        return "3-4"
    if n <= 8:
        return "5-8"
    return "9+"


def features(opnames):
    tex = sum(1 for o in opnames if o.startswith(TEX_PREFIXES))
    kill = sum(1 for o in opnames if o in KILL_OPS)
    br = sum(1 for o in opnames if o in BRANCH_OPS)
    deriv = sum(1 for o in opnames if o in DERIV_OPS)
    return {
        "tex": tex, "kill": kill, "branch": br, "deriv": deriv,
        "sig": f"tex{bucket(tex)}|kill{int(kill > 0)}|br{bucket(br)}|deriv{int(deriv > 0)}",
    }


def sha1s(text):
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:16]


def load_predict():
    table = {}
    with open(PREDICT, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            table[parts[0].lower()] = (int(parts[1]), parts[2])
    return table


def analyse_folder(cache, args, spirv_dis, predict, out_dir, work):
    tag = cache.name
    print(f"== {tag}")
    f, entries = read_pack_index(cache / PACK_NAME)
    print(f"   pack index: {len(entries)} variants")
    # Pass 1: keep Fragment variants, read their SPIR-V once per unique SHA-1.
    pixel = []  # (hash_hex, perm, sha_bin, instr_words_sha)
    uniq = {}
    stage_hist = collections.Counter()
    for h, perm, off, msz, ssz in entries:
        f.seek(off + msz)
        spv = f.read(ssz)
        words = struct.unpack(f"<{len(spv) // 4}I", spv)
        model = exec_model(words)
        stage_hist[model] += 1
        if model != FRAGMENT:
            continue
        sha = hashlib.sha1(spv).hexdigest()
        if sha not in uniq:
            uniq[sha] = spv
        pixel.append((f"{h:016x}", perm, sha))
    f.close()
    print(f"   stages: {dict(stage_hist)}; fragment variants {len(pixel)}, unique SPIR-V {len(uniq)}")

    # Loose-file cross-check (the loose 0x<hash>_<perm>.spv are a subset of the pack, per the port's loader).
    pack_keys = {(h, p) for h, p, _ in pixel}
    loose = [p.name for p in cache.glob("0x*_*.spv")]
    loose_keys = set()
    for name in loose:
        m = re.fullmatch(r"0x([0-9a-f]+)_(\d+)\.spv", name)
        if m:
            loose_keys.add((m.group(1).zfill(16), int(m.group(2))))
    print(f"   loose .spv files: {len(loose)}; loose keys not in pack: {len(loose_keys - set(all_pack_keys(cache)))}")

    # Pass 2: disassemble + normalize unique modules (parallel spirv-dis).
    def work_one(item):
        sha, spv = item
        dis = disasm(spirv_dis, spv, sha, work)
        e = normalize(dis, "E")
        c = normalize(dis, "C")
        cd = normalize(dis, "CD")
        s = normalize(dis, "S")
        names = opnames_of(e)
        return sha, {
            "E": sha1s(e), "C": sha1s(c), "CD": sha1s(cd), "S": sha1s(s),
            "n": len([x for x in names if x]), "feat": features(names), "text_e": e, "text_c": c,
        }

    results = {}
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for i, (sha, info) in enumerate(ex.map(work_one, uniq.items()), 1):
            results[sha] = info
            if i % 1000 == 0:
                print(f"   disassembled {i}/{len(uniq)}")
    print(f"   normalized {len(results)} modules")

    # Pass 3: assemble per-variant rows.
    rows = []
    for h, perm, sha in pixel:
        r = results[sha]
        pt = predict.get(h)
        rows.append({
            "hash": h, "perm": perm, "sha": sha, "pass": pt[1] if pt else "?",
            "footer": pt[0] if pt else -1, "in_predict": pt is not None, **{k: r[k] for k in ("E", "C", "CD", "S", "n")},
            "sig": r["feat"]["sig"], "tex": r["feat"]["tex"], "kill": r["feat"]["kill"],
            "branch": r["feat"]["branch"], "deriv": r["feat"]["deriv"],
        })
    rows.sort(key=lambda x: (x["pass"], x["hash"], x["perm"]))
    tsv = out_dir / f"pixel_variants_{tag}.tsv"
    with open(tsv, "w", encoding="utf-8") as fo:
        fo.write("hash\tperm\tpass\tin_predict\tinstr\tE\tC\tCD\tS\tsig\ttex\tkill\tbranch\tderiv\tsha\n")
        for r in rows:
            fo.write("\t".join(str(r[k]) for k in ("hash", "perm", "pass", "in_predict", "n", "E", "C", "CD", "S",
                                                  "sig", "tex", "kill", "branch", "deriv", "sha")) + "\n")
    print(f"   wrote {tsv}")

    summary = summarize(rows, results, tag)
    return summary, rows, results


def all_pack_keys(cache):
    f, entries = read_pack_index(cache / PACK_NAME)
    f.close()
    return [(f"{h:016x}", p) for h, p, *_ in entries]


def summarize(rows, results, tag):
    total_instr = sum(r["n"] for r in rows)
    out = {"folder": tag, "variants": len(rows), "programs": len({r["hash"] for r in rows}),
           "total_instr": total_instr, "unique_sha": len({r["sha"] for r in rows})}
    for lvl in ("E", "C", "CD", "S"):
        groups = collections.defaultdict(list)
        for r in rows:
            groups[r[lvl]].append(r)
        instr_unique = sum(g[0]["n"] for g in groups.values())
        out[f"distinct_{lvl}"] = len(groups)
        out[f"instr_redundant_share_{lvl}"] = round(1 - instr_unique / total_instr, 4) if total_instr else 0
    # Per pass, per level distinct counts.
    per_pass = {}
    for p in sorted({r["pass"] for r in rows}, key=lambda x: -sum(1 for r in rows if r["pass"] == x)):
        prs = [r for r in rows if r["pass"] == p]
        d = {"variants": len(prs), "programs": len({r["hash"] for r in prs}),
             "instr": sum(r["n"] for r in prs)}
        for lvl in ("E", "C", "CD", "S"):
            d[f"distinct_{lvl}"] = len({r[lvl] for r in prs})
        # Instruction-weighted: the instructions of the largest C-group over total.
        cgroups = collections.Counter(r["C"] for r in prs)
        d["top_C_group"] = cgroups.most_common(1)[0][1] if cgroups else 0
        per_pass[p] = d
    out["per_pass"] = per_pass
    # Biggest groups.
    for lvl in ("E", "C"):
        groups = collections.defaultdict(list)
        for r in rows:
            groups[r[lvl]].append(r)
        top = sorted(groups.values(), key=lambda g: -len(g))[:15]
        out[f"top_{lvl}_groups"] = [{
            "size": len(g), "programs": len({r["hash"] for r in g}), "instr": g[0]["n"],
            "passes": dict(collections.Counter(r["pass"] for r in g).most_common(4)), "rep": g[0]["hash"],
            "sig": g[0]["sig"]} for g in top]
    # Feature signatures.
    sigs = collections.Counter(r["sig"] for r in rows)
    out["sig_top"] = sigs.most_common(20)
    out["sig_distinct"] = len(sigs)
    # Per-program: distinct E shapes across perms.
    per_prog = collections.defaultdict(set)
    per_prog_n = collections.Counter()
    for r in rows:
        per_prog[r["hash"]].add(r["E"])
        per_prog_n[r["hash"]] += 1
    out["perm_hist"] = dict(collections.Counter(per_prog_n.values()).most_common(12))
    out["programs_multi_E"] = sum(1 for v in per_prog.values() if len(v) > 1)
    return out


def constant_analysis(rows, results, max_groups=None):
    """For C-groups whose members differ in E: which constant lines vary and their values."""
    groups = collections.defaultdict(list)
    for r in rows:
        groups[r["C"]].append(r)
    varying_lines = collections.Counter()
    samples = []
    total_groups = 0
    total_varying_lines = 0
    vals_per_line = []
    kind = collections.Counter()
    for key, members in groups.items():
        e_set = {m["E"] for m in members}
        if len(e_set) < 2:
            continue
        total_groups += 1
        texts = [results[m["sha"]]["text_e"].splitlines() for m in members]
        ctext = results[members[0]["sha"]]["text_c"].splitlines()
        varying = []
        for i, line in enumerate(ctext):
            if not (line.startswith("%") and "=" in line):
                continue
            m = RESULT_RE.match(line)
            if not m or m.group(2) not in CONST_OPS:
                continue
            vals = {t[i] if i < len(t) else "" for t in texts}
            if len(vals) > 1:
                varying.append((i, sorted(vals)))
        total_varying_lines += len(varying)
        for i, vals in varying:
            m = RESULT_RE.match(texts[0][i])
            typ = texts[0][i].split()[3] if len(texts[0][i].split()) > 3 else "?"
            kind[typ] += 1
            varying_lines[len(vals)] += 1
            vals_per_line.append(len(vals))
        if max_groups is None or len(samples) < max_groups:
            samples.append({"members": len(members), "varying_const_lines": len(varying),
                            "pass": members[0]["pass"], "sample": [(texts[0][i], v[:6]) for i, v in varying[:6]]})
    return {"groups_with_constant_only_diffs": total_groups, "varying_constant_lines": total_varying_lines,
            "value_set_sizes": dict(varying_lines.most_common(10)), "const_op_lines": dict(kind.most_common(8)),
            "samples": samples[:12]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", type=Path, action="append")
    ap.add_argument("--work", type=Path,
                    default=Path(os.environ.get("TEMP", "/tmp")) / "pixel_dupes_work")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()
    caches = args.cache or DEFAULT_CACHES
    out_dir = args.out or args.work
    args.work.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    spirv_dis = find_spirv_dis()
    predict = load_predict()
    print(f"predict table: {len(predict)} programs")
    full = {}
    for cache in caches:
        summary, rows, results = analyse_folder(cache, args, spirv_dis, predict, out_dir, args.work)
        summary["constants"] = constant_analysis(rows, results)
        full[cache.name] = summary
        print(json.dumps({k: v for k, v in summary.items() if k not in ("per_pass",)}, indent=1)[:6000])
        print("per pass (top 12 by variants):")
        for p, d in list(summary["per_pass"].items())[:12]:
            print(f"   {p:32s} var {d['variants']:5d} prog {d['programs']:5d} E {d['distinct_E']:5d} "
                  f"C {d['distinct_C']:5d} CD {d['distinct_CD']:5d} S {d['distinct_S']:4d}")
    with open(out_dir / "pixel_summary.json", "w", encoding="utf-8") as fo:
        json.dump(full, fo, indent=1, default=str)
    print(f"summary: {out_dir / 'pixel_summary.json'}")


if __name__ == "__main__":
    main()
