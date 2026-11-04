#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export order and export content of the GBuffer pixel shaders (RE, read-only).

Input: a SOTCPRE1 pre-pack (format as in tools/re/pixel_dupes.py) and user/shader_predict/predict_table.tsv.
For every Fragment module whose predicted pass name matches --pass-regex, the script:
  1. disassembles it with spirv-dis (cached per SHA-1 in --work);
  2. lists the Fragment outputs (Location, name, declared type, stored components);
  3. prints the expression that each stored component is computed from (depth-limited);
  4. summarises the leaves of every output: input attributes, buffer loads, texture samples, ops used.

Output: a per-module report (--report) and an aggregate table on stdout: for each pass and output Location,
the declared type, the stored components and the number of modules that agree.

Usage:
  python tools/re/gbuffer_exports.py [--cache DIR] [--pass-regex gbuffer] [--sample N] [--depth 4]
                                     [--work DIR] [--report FILE] [--json FILE]

Requires spirv-dis (MSYS2 CLANG64). Stdlib only.
"""

import argparse
import collections
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
DEFAULT_CACHE = REPO / "user/cache/CUSA08809_v13272bcc"
PREDICT = REPO / "user/shader_predict/predict_table.tsv"
DEFAULT_WORK = REPO / "out/re/gbuffer_exports"
PACK_NAME = "sotc_pretranslate.pack"
PACK_MAGIC = b"SOTCPRE1"
FRAGMENT = 4
OP_ENTRY_POINT = 15

RESULT_RE = re.compile(r"^%(\d+) = (\S+)(?: (.*))?$")
PLAIN_RE = re.compile(r"^(\S+)(?: (.*))?$")
NAME_RE = re.compile(r'^%(\d+) "(.*)"$')
ACCESS_OPS = {"OpAccessChain", "OpInBoundsAccessChain"}
INFIX = {"OpFAdd": "+", "OpFSub": "-", "OpFMul": "*", "OpFDiv": "/", "OpIAdd": "+", "OpISub": "-",
         "OpIMul": "*", "OpBitwiseAnd": "&", "OpBitwiseOr": "|", "OpBitwiseXor": "^",
         "OpShiftLeftLogical": "<<", "OpShiftRightLogical": ">>", "OpShiftRightArithmetic": ">>a",
         "OpLogicalAnd": "&&", "OpLogicalOr": "||", "OpFOrdLessThan": "<", "OpFOrdGreaterThan": ">",
         "OpFOrdLessThanEqual": "<=", "OpFOrdGreaterThanEqual": ">=", "OpULessThan": "<u",
         "OpIEqual": "==", "OpINotEqual": "!="}
UNARY = {"OpFNegate": "-", "OpLogicalNot": "!", "OpNot": "~"}
CONVERT = {"OpConvertFToU": "u", "OpConvertFToS": "i", "OpConvertUToF": "f", "OpConvertSToF": "f",
           "OpBitcast": "bitcast"}
SAMPLE_PREFIX = "OpImageSample"
MAX_CONE = 20000


def find_spirv_dis():
    found = shutil.which("spirv-dis")
    if found:
        return found
    for cand in ("C:/msys64/clang64/bin/spirv-dis.exe", "/clang64/bin/spirv-dis.exe",
                 "/i/msys64/clang64/bin/spirv-dis.exe"):
        if Path(cand).exists():
            return cand
    sys.exit("spirv-dis not found (MSYS2 CLANG64 spirv-tools)")


def read_pack_index(f):
    f.seek(-16, os.SEEK_END)
    (index_offset,) = struct.unpack("<Q", f.read(8))
    if f.read(8) != PACK_MAGIC:
        raise SystemExit("bad pack trailer magic")
    f.seek(index_offset)
    (count,) = struct.unpack("<I", f.read(4))
    return [struct.unpack("<QIQII", f.read(28)) for _ in range(count)]


def exec_model(words):
    i = 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0:
            break
        if op == OP_ENTRY_POINT:
            return words[i + 1]
        i += wc
    return None


def load_predict(pass_re):
    """Map program hash -> pass name for Fragment (footer type 0) programs whose pass matches pass_re."""
    table = {}
    with open(PREDICT, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3 or parts[1] != "0":
                continue
            if pass_re.search(parts[2]):
                table.setdefault(int(parts[0], 16), set()).add(parts[2])
    return table


def collect_modules(cache, pass_re):
    """Return {sha1: (spv_bytes, [(hash, perm, pass), ...])} for the matching Fragment modules."""
    predict = load_predict(pass_re)
    mods = {}
    with open(cache / PACK_NAME, "rb") as f:
        for h, perm, off, msz, ssz in read_pack_index(f):
            passes = predict.get(h)
            if not passes:
                continue
            f.seek(off + msz)
            spv = f.read(ssz)
            words = struct.unpack(f"<{len(spv) // 4}I", spv)
            if exec_model(words) != FRAGMENT:
                continue
            sha = hashlib.sha1(spv).hexdigest()
            entry = mods.setdefault(sha, (spv, []))
            for p in passes:
                entry[1].append((f"{h:016x}", perm, p))
    return mods


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


class Module:
    """Parsed view of one disassembled SPIR-V module (defs, names, decorations, types, stores)."""

    def __init__(self, text):
        self.defs = {}          # id -> (opcode, operand text)
        self.names = {}         # id -> debug name
        self.loc = {}           # id -> Location
        self.binding = {}       # id -> Binding
        self.builtin = {}       # id -> BuiltIn name (FragCoord, BaryCoordKHR, ...)
        self.vars = {}          # id -> (pointer type id, storage class)
        self.types = {}         # id -> (kind, args...)
        self.consts = {}        # id -> literal text
        self.stores = []        # (pointer id, value id) in program order
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith(";"):
                continue
            m = RESULT_RE.match(line)
            if m:
                rid, op, rest = m.group(1), m.group(2), m.group(3) or ""
            else:
                m = PLAIN_RE.match(line)
                rid, op, rest = None, m.group(1), m.group(2) or ""
            toks = rest.split()
            if op == "OpName" and rid is None:
                n = NAME_RE.match(rest)
                if n:
                    self.names[n.group(1)] = n.group(2)
                continue
            if op == "OpDecorate" and rid is None:
                ident = toks[0].lstrip("%")
                if len(toks) >= 3 and toks[1] == "Location":
                    self.loc[ident] = int(toks[2])
                if len(toks) >= 3 and toks[1] == "Binding":
                    self.binding[ident] = int(toks[2])
                if len(toks) >= 3 and toks[1] == "BuiltIn":
                    self.builtin[ident] = toks[2]
                continue
            if op == "OpStore":
                self.stores.append((toks[0].lstrip("%"), toks[1].lstrip("%")))
                continue
            if rid is None:
                continue
            self.defs[rid] = (op, rest)
            if op == "OpVariable":
                self.vars[rid] = (toks[0].lstrip("%"), toks[1])
            elif op == "OpTypePointer":
                self.types[rid] = ("ptr", toks[0], toks[1].lstrip("%"))
            elif op == "OpTypeVector":
                self.types[rid] = ("vec", toks[0].lstrip("%"), int(toks[1]))
            elif op == "OpTypeFloat":
                self.types[rid] = ("float", int(toks[0]))
            elif op == "OpTypeInt":
                self.types[rid] = ("int", int(toks[0]), toks[1] == "1")
            elif op in ("OpConstant", "OpSpecConstant"):
                self.consts[rid] = toks[1]
            elif op == "OpConstantTrue":
                self.consts[rid] = "true"
            elif op == "OpConstantFalse":
                self.consts[rid] = "false"

    # --- helpers -------------------------------------------------------------------------------

    def label(self, vid):
        return self.names.get(vid, vid)

    def const_text(self, tok):
        tok = tok.lstrip("%")
        return self.consts.get(tok, tok) if tok in self.consts else f"%{tok}"

    def resolve_ptr(self, pid):
        """Follow access chains to (base id, [index texts])."""
        chain = []
        cur = pid
        while cur in self.defs and self.defs[cur][0] in ACCESS_OPS:
            toks = self.defs[cur][1].split()
            chain = [self.const_text(t) for t in toks[2:]] + chain
            cur = toks[1].lstrip("%")
        return cur, chain

    def var_storage(self, vid):
        return self.vars.get(vid, (None, None))[1]

    def value_type(self, vid):
        """Text of the declared type of a variable's pointee (for outputs/inputs)."""
        if vid not in self.vars:
            return "?"
        ptr = self.types.get(self.vars[vid][0])
        if not ptr:
            return "?"
        return self.type_text(ptr[2])

    def type_text(self, tid):
        t = self.types.get(tid)
        if not t:
            return "?"
        if t[0] == "vec":
            return f"{self.type_text(t[1])}x{t[2]}"
        if t[0] == "float":
            return f"f{t[1]}"
        if t[0] == "int":
            return f"{'i' if t[2] else 'u'}{t[1]}"
        return t[0]

    def tex_name(self, sampled_id):
        """Texture and sampler names behind an OpSampledImage (or an image load)."""
        if sampled_id in self.defs and self.defs[sampled_id][0] == "OpSampledImage":
            toks = self.defs[sampled_id][1].split()
            return f"{self.image_name(toks[1].lstrip('%'))}+{self.image_name(toks[2].lstrip('%'))}"
        return self.image_name(sampled_id)

    def image_name(self, vid):
        if vid in self.defs and self.defs[vid][0] == "OpLoad":
            vid = self.defs[vid][1].split()[1].lstrip("%")
        base, _ = self.resolve_ptr(vid)
        name = self.label(base)
        if base in self.binding:
            name += f"(b{self.binding[base]})"
        return name

    # --- expression text -----------------------------------------------------------------------

    def load_text(self, ptr_id):
        base, chain = self.resolve_ptr(ptr_id)
        suffix = "".join(f"[{c}]" for c in chain)
        storage = self.var_storage(base)
        if base in self.builtin:
            return f"{self.builtin[base]}{suffix}"
        if storage == "Input" and base in self.loc:
            return f"in{self.loc[base]}{suffix}"
        if storage in ("StorageBuffer", "Uniform", "PushConstant"):
            return f"buf:{self.label(base)}{suffix}"
        if storage == "UniformConstant":
            return f"img:{self.image_name(base)}"
        return f"{self.label(base)}{suffix}"

    def expr(self, vid, depth, budget):
        """Depth-limited expression text. budget is a one-element list of remaining nodes."""
        if vid in self.consts:
            return self.consts[vid]
        if vid not in self.defs:
            return self.const_text(vid)
        budget[0] -= 1
        if budget[0] < 0:
            return "..."
        op, rest = self.defs[vid]
        toks = rest.split()
        if op == "OpLoad":
            return self.load_text(toks[1].lstrip("%"))
        if depth <= 0:
            return f"{op}(...)"
        def arg(t):
            return self.expr(t.lstrip("%"), depth - 1, budget)
        if op in ACCESS_OPS:
            base, chain = self.resolve_ptr(vid)
            return f"&{self.label(base)}{''.join('[%s]' % c for c in chain)}"
        if op in INFIX:
            return f"({arg(toks[1])} {INFIX[op]} {arg(toks[2])})"
        if op in UNARY:
            return f"{UNARY[op]}{arg(toks[1])}"
        if op in CONVERT:
            return f"{CONVERT[op]}({arg(toks[1])})"
        if op == "OpExtInst":
            name = toks[2]
            return f"{name}({', '.join(arg(t) for t in toks[3:])})"
        if op == "OpCompositeExtract":
            idx = ".".join(toks[2:])
            return f"{arg(toks[1])}.{idx}"
        if op == "OpCompositeConstruct":
            return f"vec({', '.join(arg(t) for t in toks[1:])})"
        if op == "OpVectorShuffle":
            return f"shuffle({arg(toks[1])}, {arg(toks[2])}, {' '.join(toks[3:])})"
        if op == "OpBitFieldInsert":
            return (f"insert({arg(toks[1])}, {arg(toks[2])}, off={self.const_text(toks[3])}, "
                    f"n={self.const_text(toks[4])})")
        if op == "OpBitFieldUExtract" or op == "OpBitFieldSExtract":
            return (f"extract({arg(toks[1])}, off={self.const_text(toks[2])}, "
                    f"n={self.const_text(toks[3])})")
        if op == "OpSelect":
            return f"select({arg(toks[1])}, {arg(toks[2])}, {arg(toks[3])})"
        if op.startswith(SAMPLE_PREFIX) or op.startswith("OpImageFetch") or op.startswith("OpImageGather"):
            tex = self.tex_name(toks[1].lstrip("%"))
            coord = arg(toks[2]) if len(toks) > 2 else "?"
            return f"{op[2:]}[{tex}]({coord})"
        if op == "OpDot":
            return f"dot({arg(toks[1])}, {arg(toks[2])})"
        if op == "OpIsNan":
            return f"isnan({arg(toks[1])})"
        if op == "OpPhi":
            return "phi(" + ", ".join(arg(t) for t in toks[1::2] if t.startswith("%")) + ")"
        return f"{op}({', '.join(arg(t) for t in toks[1:] if t.startswith('%'))})"

    # --- data flow -----------------------------------------------------------------------------

    def _note_source(self, ptr_id, info):
        base, chain = self.resolve_ptr(ptr_id)
        suffix = "".join(f"[{c}]" for c in chain)
        storage = self.var_storage(base)
        if base in self.builtin:
            info["inputs"].add(f"{self.builtin[base]}{suffix}")
        elif storage == "Input" and base in self.loc:
            info["inputs"].add(f"in{self.loc[base]}{suffix}")
        elif storage in ("StorageBuffer", "Uniform", "PushConstant"):
            info["buffers"].add(f"{self.label(base)}{suffix}")

    def pack_fields(self, vid):
        """Peel OpBitFieldInsert (through bitcasts): returns [(offset, count, field id)] and the base id."""
        fields = []
        cur = vid
        while cur in self.defs:
            op, rest = self.defs[cur]
            toks = rest.split()
            if op == "OpBitcast":
                cur = toks[1].lstrip("%")
                continue
            if op != "OpBitFieldInsert":
                break
            fields.append((self.const_text(toks[3]), self.const_text(toks[4]), toks[2].lstrip("%")))
            cur = toks[1].lstrip("%")
        fields.reverse()
        return fields, cur

    def cone(self, vid):
        """All defining instructions reachable from vid (bounded). Returns a summary dict."""
        seen = set()
        stack = [vid]
        info = {"inputs": set(), "buffers": set(), "textures": set(), "texchan": set(),
                "ops": collections.Counter(), "ext": collections.Counter(), "nodes": 0}
        while stack and len(seen) < MAX_CONE:
            cur = stack.pop()
            if cur in seen or cur in self.consts or cur not in self.defs:
                continue
            seen.add(cur)
            op, rest = self.defs[cur]
            toks = rest.split()
            info["ops"][op] += 1
            if op == "OpExtInst":
                info["ext"][toks[2]] += 1
                ids = [t for t in toks[3:] if t.startswith("%")]
            elif op == "OpLoad":
                self._note_source(toks[1].lstrip("%"), info)
                ids = []
            elif op in ACCESS_OPS:
                self._note_source(cur, info)
                ids = [t for t in toks[2:] if t.startswith("%")]
            elif op.startswith(SAMPLE_PREFIX) or op.startswith("OpImageFetch"):
                info["textures"].add(self.tex_name(toks[1].lstrip("%")))
                ids = [t for t in toks[2:] if t.startswith("%")]
            elif op == "OpCompositeExtract":
                src = toks[1].lstrip("%")
                if src in self.defs and self.defs[src][0].startswith(SAMPLE_PREFIX):
                    srest = self.defs[src][1]
                    info["texchan"].add(f"{self.tex_name(srest.split()[1].lstrip('%'))}.{toks[2]}")
                ids = [t for t in toks[1:2] if t.startswith("%")]
            else:
                ids = [t for t in toks[1:] if t.startswith("%")]
            stack.extend(i.lstrip("%") for i in ids)
        info["nodes"] = len(seen)
        return info


FIELD_DEPTH = 4
FIELD_BUDGET = 600


def analyse(mod):
    """Return the list of Fragment outputs (dicts) with their stored components and cone summaries."""
    outputs = []
    by_var = collections.defaultdict(list)
    for ptr, val in mod.stores:
        base, chain = mod.resolve_ptr(ptr)
        if mod.var_storage(base) == "Output" and base in mod.loc:
            comp = chain[-1] if chain else "all"
            by_var[base].append((comp, val))
    for vid in sorted(mod.vars, key=lambda v: (mod.loc.get(v, 999), int(v))):
        if mod.var_storage(vid) != "Output" or vid not in mod.loc:
            continue
        stores = by_var.get(vid, [])
        comps = []
        for comp, val in stores:
            budget = [4000]
            entry = {"component": comp, "expr": mod.expr(val, 5, budget), "cone": mod.cone(val), "fields": []}
            fields, base = mod.pack_fields(val)
            for off, cnt, fid in fields:
                entry["fields"].append({"offset": off, "count": cnt,
                                        "expr": mod.expr(fid, FIELD_DEPTH, [FIELD_BUDGET]),
                                        "top": mod.defs.get(fid, ("?",))[0],
                                        "cone": mod.cone(fid)})
            entry["base"] = mod.expr(base, 3, [400]) if fields else None
            comps.append(entry)
        outputs.append({"location": mod.loc[vid], "name": mod.label(vid), "declared": mod.value_type(vid),
                        "stored": sorted({c for c, _ in stores}, key=str), "components": comps})
    return outputs


def inputs_of(mod):
    rows = []
    for vid in sorted(mod.vars, key=lambda v: (mod.loc.get(v, 999), int(v))):
        if mod.var_storage(vid) == "Input" and vid in mod.loc:
            rows.append((mod.loc[vid], mod.label(vid), mod.value_type(vid)))
    return rows


def describe_module(sha, meta, outputs, inputs):
    lines = [f"== module {sha[:16]} passes={sorted({p for _, _, p in meta})} "
             f"hashes={sorted({h for h, _, _ in meta})[:3]} perms={sorted({perm for _, perm, _ in meta})}"]
    lines.append("   inputs: " + ", ".join(f"loc{l}:{n}:{t}" for l, n, t in inputs))
    for out in outputs:
        lines.append(f"   loc{out['location']} {out['name']} declared={out['declared']} stored={out['stored']}")
        for c in out["components"]:
            cone = c["cone"]
            lines.append(f"      [{c['component']}] {c['expr']}")
            lines.append(f"         inputs={sorted(cone['inputs'])} buffers={sorted(cone['buffers'])[:8]} "
                         f"textures={sorted(cone['textures'])}")
            for fld in c["fields"]:
                fc = fld["cone"]
                lines.append(f"         field@{fld['offset']} n={fld['count']} top={fld['top']}: {fld['expr']}")
                lines.append(f"            tex={sorted(fc['textures'])} texchan={sorted(fc['texchan'])[:12]} "
                             f"inputs={sorted(fc['inputs'])[:12]} buffers={sorted(fc['buffers'])[:8]} "
                             f"ext={dict(fc['ext'])}")
            if c["fields"]:
                lines.append(f"         packed base: {c['base']}")
            ext = dict(cone["ext"])
            interesting = {k: v for k, v in cone["ops"].items()
                           if k in ("OpBitFieldInsert", "OpConvertFToU", "OpImageSampleImplicitLod", "OpFMul")}
            lines.append(f"         ext={ext} ops={interesting} nodes={cone['nodes']}")
    return "\n".join(lines)


def signature(outputs):
    """Compact per-module signature: location -> declared type and stored component set."""
    return tuple((o["location"], o["name"], o["declared"], tuple(map(str, o["stored"]))) for o in outputs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    ap.add_argument("--pass-regex", default="gbuffer")
    ap.add_argument("--sample", type=int, default=24, help="modules to detail (evenly spaced); 0 = all")
    ap.add_argument("--field-depth", type=int, default=4, help="expression depth of each packed field")
    ap.add_argument("--field-budget", type=int, default=600, help="expression node budget of each packed field")
    ap.add_argument("--work", type=Path, default=DEFAULT_WORK)
    ap.add_argument("--report", type=Path, default=None)
    ap.add_argument("--json", type=Path, default=None)
    args = ap.parse_args()
    global FIELD_DEPTH, FIELD_BUDGET
    FIELD_DEPTH, FIELD_BUDGET = args.field_depth, args.field_budget

    args.work.mkdir(parents=True, exist_ok=True)
    spirv_dis = find_spirv_dis()
    mods = collect_modules(args.cache, re.compile(args.pass_regex))
    print(f"fragment modules matching /{args.pass_regex}/: {len(mods)} unique SPIR-V")

    shas = sorted(mods)
    if args.sample and len(shas) > args.sample:
        step = len(shas) / args.sample
        shas = [shas[int(i * step)] for i in range(args.sample)]

    report_chunks = []
    records = []
    sig_count = collections.Counter()
    sig_passes = collections.defaultdict(collections.Counter)
    for sha in shas:
        spv, meta = mods[sha]
        text = disasm(spirv_dis, spv, sha, args.work)
        mod = Module(text)
        outputs = analyse(mod)
        inputs = inputs_of(mod)
        report_chunks.append(describe_module(sha, meta, outputs, inputs))
        sig = signature(outputs)
        sig_count[sig] += 1
        for p in {p for _, _, p in meta}:
            sig_passes[sig][p] += 1
        records.append({"sha1": sha, "passes": sorted({p for _, _, p in meta}),
                        "outputs": [{k: v for k, v in o.items() if k != "components"} |
                                    {"exprs": [c["expr"] for c in o["components"]]} for o in outputs]})

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text("\n\n".join(report_chunks) + "\n", encoding="utf-8")
        print(f"detail written to {args.report}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(records, indent=1), encoding="utf-8")

    print(f"\nexport signatures over {len(shas)} modules (location, name, declared type, stored components):")
    for sig, n in sig_count.most_common():
        print(f"  {n:3d} modules  passes={dict(sig_passes[sig])}")
        for loc, name, decl, stored in sig:
            print(f"        loc{loc} {name:<22} {decl:<8} stored={','.join(stored)}")
    if not args.report:
        print("\n" + report_chunks[0] if report_chunks else "no modules")


if __name__ == "__main__":
    main()
