#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Code patterns of the particle emitter compute programs (emitter_<n>_spawn, emitter_<n>_update).

Reads the compute modules of the SOTCPRE1 pack (format and parser: compute_dupes.py) whose predict_table.tsv
name is an emitter spawn or update pass, and reports:

  - regions: instruction runs cut at storage/uniform/image loads and stores, image ops, atomics, barriers,
    calls, terminators and labels; and basic blocks. Each is hashed in two modes:
      shape  constant values are abstracted to their type (for example Kf32, Ki32);
      exact  constant values are kept.
    Ids are renumbered by first use inside the run, so global numbering does not matter.
    For each key: programs containing it, and the instructions it covers (occurrences x length).
    Coverage of the top N keys (ranked by program count), keys needed for 50% and 90% coverage, and the
    per-program remainder (instructions in keys that only one program has).
  - storage buffer accesses: (binding, struct field path, read/write, type) per program. Field paths use
    struct member indices; array indices are shown as [] and vector components as '.'.
  - storage block layouts (member offsets from OpMemberDecorate Offset).
  - float and int constants, RNG-like constants, GLSL.std.450 instructions and image ops per program.
  - a module-set proxy: distinct tuples of (bindings, GLSL ops, image ops, RNG tags).
  - --dump HEX --dump-dir DIR: write the SPIR-V of one program (for spirv-dis).

Output: a summary on stdout. Stdlib only. Nothing is written inside the repository.

usage:
  python -I tools/re/particle_patterns.py [--pack PACK] [--table TSV] [--grammar GRAMMAR] [--top N]
                                          [--limit N] [--dump HEX --dump-dir DIR]
"""
import argparse
import collections
import hashlib
import importlib.util
import os
import re
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("compute_dupes", os.path.join(HERE, "compute_dupes.py"))
cd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cd)

DEFAULT_PACK = cd.DEFAULT_PACK
DEFAULT_TABLE = cd.DEFAULT_PREDICT
DEFAULT_GRAMMAR = cd.DEFAULT_GRAMMAR
NAME_RE = re.compile(r"^emitter_(\d+)_(spawn|update)$")

TERMINATORS = {"OpBranch", "OpBranchConditional", "OpSwitch", "OpReturn", "OpReturnValue", "OpKill",
               "OpUnreachable"}
SPLIT_OPS = {"OpControlBarrier", "OpMemoryBarrier", "OpFunctionCall", "OpCopyMemory"}
SKIP_IN_FUNC = {"OpFunctionParameter", "OpVariable", "OpLine", "OpNoLine"}
NOT_MEM_IMAGE = {"OpImage", "OpImageTexelPointer", "OpSampledImage"}
GLOBAL_SC = {0, 2, 4, 9, 11, 12}  # UniformConstant, Uniform, Workgroup, PushConstant, Image, StorageBuffer
SC_NAME = {0: "UC", 1: "In", 2: "UBO", 3: "Out", 4: "WG", 5: "CWG", 6: "Priv", 7: "Fn", 8: "Gen", 9: "PC",
           10: "AC", 11: "Img", 12: "SSBO"}
DEC_BINDING, DEC_SET, DEC_OFFSET = 33, 34, 35
RNG_CONSTS = {
    1664525: "lcg-numrec", 1013904223: "lcg-numrec-inc", 1103515245: "lcg-ansi", 22695477: "lcg-22695477",
    747796405: "pcg-747796405", 2891336453: "pcg-2891336453", 0x9E3779B9: "golden-ratio",
    0x85EBCA6B: "fmix32-a", 0xC2B2AE35: "fmix32-b", 0x7FEB352D: "lowbias32-a", 0x846CA68B: "lowbias32-b",
    0x27D4EB2D: "xxhash-prime", 0x5BD1E995: "murmur2",
}
RNG_FLOAT_BITS = {0x2F000000: "float 2^-32 scale", 0x33800000: "float 2^-24 scale"}
GLSL_HEADER = "C:/msys64/clang64/include/spirv/unified1/GLSL.std.450.h"


def load_glsl_names(path=GLSL_HEADER):
    """GLSL.std.450 opcode number -> name, read from the SPIRV-Headers enum (GLSLstd450<Name> = <n>)."""
    names = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            for mt in re.finditer(r"GLSLstd450(\w+)\s*=\s*(\d+)", fh.read()):
                names[int(mt.group(2))] = mt.group(1)
    return names


GLSL = load_glsl_names()
DEF_OPS = {"OpIAdd", "OpISub", "OpIMul", "OpShiftLeftLogical", "OpShiftRightLogical", "OpBitwiseAnd",
           "OpBitwiseOr", "OpBitFieldUExtract", "OpLoad", "OpBitcast", "OpUConvert", "OpSConvert",
           "OpCompositeExtract"}
DESC_SYM = {"OpIAdd": "+", "OpISub": "-", "OpIMul": "*", "OpShiftLeftLogical": "<<",
            "OpShiftRightLogical": ">>", "OpBitwiseAnd": "&", "OpBitwiseOr": "|"}
LABELS = ("reg_shape", "reg_exact", "blk_shape", "blk_exact")


class Module:
    """Tables of one SPIR-V module, plus the storage access tracking used by the region split."""

    def __init__(self, ins):
        self.types = {}      # id -> (opname, operand tokens without the result id)
        self.consts = {}     # id -> (type id, kind 'L' literal / 'C' composite / 'N' null, words or member ids)
        self.decor = {}      # id -> {decoration: first parameter}
        self.memdec = {}     # (struct id, member) -> {decoration: parameter}
        self.gvars = {}      # variable id -> (storage class, pointer type id)
        self.extinst = {}    # ext set id -> name
        self.funcs = set()
        self.chain = {}      # pointer id -> (root variable, field path, pointee type id)
        self.defs = {}       # result id -> instruction, for the value ops used by index expressions
        self.tmemo = {}
        self.accesses = set()
        for i in ins:
            n = i.name
            t = i.tokens
            if n.startswith("OpType"):
                self.types[t[0][0]] = (n, t[1:])
            elif n == "OpConstant":
                self.consts[t[1][0]] = (t[0][0], "L", tuple(i.words[2:]))
            elif n in ("OpConstantTrue", "OpConstantFalse"):
                self.consts[t[1][0]] = (t[0][0], "L", (1 if n.endswith("True") else 0,))
            elif n == "OpConstantComposite":
                self.consts[t[1][0]] = (t[0][0], "C", tuple(v for v, _ in t[2:]))
            elif n == "OpConstantNull":
                self.consts[t[1][0]] = (t[0][0], "N", ())
            elif n == "OpDecorate":
                self.decor.setdefault(t[0][0], {})[t[1][0]] = t[2][0] if len(t) > 2 else None
            elif n == "OpMemberDecorate":
                self.memdec.setdefault((t[0][0], t[1][0]), {})[t[2][0]] = t[3][0] if len(t) > 3 else None
            elif n == "OpVariable":
                self.gvars[t[1][0]] = (t[2][0], t[0][0])
            elif n == "OpExtInstImport":
                raw = b"".join(struct.pack("<I", w) for w in i.words[1:])
                self.extinst[t[0][0]] = raw.split(b"\0")[0].decode("ascii", "replace")
            elif n == "OpFunction":
                self.funcs.add(t[1][0])
        for var, (sc, rt) in self.gvars.items():
            self.chain[var] = (var, (), self.pointee(rt))

    def desc(self, vid, depth=0):
        """Readable form of an index expression: constants as numbers, arithmetic kept, other values as v."""
        c = self.consts.get(vid)
        if c is not None:
            return str(c[2][0]) if c[1] == "L" and c[2] else "C"
        d = self.defs.get(vid)
        if d is None or depth > 6:
            return "v"
        if d.name in DESC_SYM:
            return "(%s%s%s)" % (self.desc(d.tokens[2][0], depth + 1), DESC_SYM[d.name],
                                 self.desc(d.tokens[3][0], depth + 1))
        if d.name == "OpBitFieldUExtract":
            return "bf(%s,%s,%s)" % (self.desc(d.tokens[2][0], depth + 1), self.desc(d.tokens[3][0], depth + 1),
                                     self.desc(d.tokens[4][0], depth + 1))
        if d.name == "OpLoad":
            return "ld"
        return "v"

    def pointee(self, ptr):
        n, t = self.types.get(ptr, ("?", []))
        return t[1][0] if n == "OpTypePointer" else None

    def tstr(self, tid):
        m = self.tmemo.get(tid)
        if m is not None:
            return m
        if tid not in self.types:
            return "?"
        self.tmemo[tid] = "?"  # recursion guard
        n, t = self.types[tid]
        if n == "OpTypeVoid":
            s = "void"
        elif n == "OpTypeBool":
            s = "bool"
        elif n == "OpTypeInt":
            s = "i%d%s" % (t[0][0], "" if t[1][0] else "u")
        elif n == "OpTypeFloat":
            s = "f%d" % t[0][0]
        elif n == "OpTypeVector":
            s = "vec%d<%s>" % (t[1][0], self.tstr(t[0][0]))
        elif n == "OpTypeMatrix":
            s = "mat%d<%s>" % (t[1][0], self.tstr(t[0][0]))
        elif n == "OpTypeArray":
            s = "arr<%s>" % self.tstr(t[0][0])
        elif n == "OpTypeRuntimeArray":
            s = "rta<%s>" % self.tstr(t[0][0])
        elif n == "OpTypeStruct":
            s = "{%s}" % ",".join(self.tstr(v) for v, _ in t)
        elif n == "OpTypePointer":
            s = "ptr<%s,%s>" % (SC_NAME.get(t[0][0], t[0][0]), self.tstr(t[1][0]))
        elif n == "OpTypeImage":
            s = "img%d" % t[1][0]
        elif n == "OpTypeSampledImage":
            s = "simg"
        else:
            s = n[6:].lower()
        self.tmemo[tid] = s
        return s

    def cval(self, cid):
        c = self.consts.get(cid)
        if c is None:
            return "?"
        if c[1] == "L":
            return "".join("%08x" % w for w in c[2])
        if c[1] == "C":
            return "{%s}" % ",".join(self.cval(m) for m in c[2])
        return "null"

    def bindkey(self, var):
        sc, _ = self.gvars[var]
        d = self.decor.get(var, {})
        if DEC_BINDING in d:
            return "s%s.b%s" % (d.get(DEC_SET), d[DEC_BINDING])
        return SC_NAME.get(sc, "sc%s" % sc)

    def idname(self, val, exact, local):
        if val in self.types:
            return "T" + self.tstr(val)
        c = self.consts.get(val)
        if c is not None:
            return "K" + self.tstr(c[0]) + ("=" + self.cval(val) if exact else "")
        if val in self.gvars:
            return "G" + self.bindkey(val)
        if val in self.funcs:
            return "F"
        if val in self.extinst:
            return "X" + self.extinst[val]
        loc = local.get(val)
        if loc is None:
            loc = "%%%d" % len(local)
            local[val] = loc
        return loc

    def key(self, insns, exact):
        local = {}
        lines = []
        for i in insns:
            parts = [i.name]
            for val, isid in i.tokens:
                parts.append(self.idname(val, exact, local) if isid else str(val))
            lines.append(" ".join(parts))
        digest = hashlib.sha1("\n".join(lines).encode("utf-8")).hexdigest()[:16]
        return int(digest, 16), lines

    def layout(self, tid):
        n, t = self.types.get(tid, ("?", []))
        if n == "OpTypeStruct":
            parts = []
            for idx, (mid, _) in enumerate(t):
                off = self.memdec.get((tid, idx), {}).get(DEC_OFFSET)
                parts.append("%s@%s" % (self.layout(mid), off))
            return "{%s}" % ",".join(parts)
        if n in ("OpTypeArray", "OpTypeRuntimeArray"):
            return "[%s]" % self.layout(t[0][0])
        return self.tstr(tid)

    # Storage access tracking (called in instruction order inside functions).
    def track(self, i):
        n = i.name
        t = i.tokens
        if n in ("OpAccessChain", "OpInBoundsAccessChain"):
            rec = self.chain.get(t[2][0])
            if rec is None:
                return
            root, path, cur = rec
            path = list(path)
            for idv, _ in t[3:]:
                typ = self.types.get(cur)
                if typ is None:
                    break
                tn, tt = typ
                if tn == "OpTypeStruct":
                    c = self.consts.get(idv)
                    if c is None or c[1] != "L":
                        path.append("*")
                        break
                    v = c[2][0]
                    path.append(v)
                    cur = tt[v][0] if v < len(tt) else None
                elif tn in ("OpTypeArray", "OpTypeRuntimeArray"):
                    path.append("[%s]" % self.desc(idv))
                    cur = tt[0][0]
                elif tn in ("OpTypeVector", "OpTypeMatrix"):
                    path.append(".")
                    cur = tt[0][0]
                else:
                    break
            self.chain[t[1][0]] = (root, tuple(path), cur)
        elif n == "OpLoad" or n == "OpStore":
            ptr = t[2][0] if n == "OpLoad" else t[0][0]
            rec = self.chain.get(ptr)
            if rec is not None and self.gvars[rec[0]][0] in GLOBAL_SC:
                ty = self.tstr(t[0][0]) if n == "OpLoad" else self.tstr(rec[2])
                path = ".".join(str(p) for p in rec[1]) or "-"
                self.accesses.add((self.bindkey(rec[0]), path, "R" if n == "OpLoad" else "W", ty))

    def splits(self, i):
        n = i.name
        if n in SPLIT_OPS or n.startswith("OpAtomic"):
            return True
        if n.startswith("OpImage") and n not in NOT_MEM_IMAGE:
            return True
        if n == "OpLoad" or n == "OpStore":
            ptr = i.tokens[2][0] if n == "OpLoad" else i.tokens[0][0]
            rec = self.chain.get(ptr)
            return rec is not None and self.gvars[rec[0]][0] in GLOBAL_SC
        return False


def cut(buf, out):
    if buf:
        out.append(list(buf))
        del buf[:]


def analyze(gr, ins):
    """Everything the summary needs about one program."""
    m = Module(ins)
    regions, blocks = [], []
    cur_r, cur_b = [], []
    in_func = False
    exts = set()
    imgops = set()
    opcount = collections.Counter()
    for i in ins:
        n = i.name
        if n not in cd.DROP_OPS:
            opcount[n] += 1
        if n == "OpExtInst":
            setname = m.extinst.get(i.tokens[2][0], "?")
            num = i.tokens[3][0]
            exts.add(GLSL.get(num, "op%d" % num) if setname.startswith("GLSL") else "%s#%d" % (setname, num))
        elif n.startswith("OpImage") and n not in NOT_MEM_IMAGE:
            imgops.add(n)
        if n == "OpFunction":
            in_func = True
            continue
        if n == "OpFunctionEnd":
            in_func = False
            cut(cur_r, regions)
            cut(cur_b, blocks)
            continue
        if not in_func:
            continue
        if n == "OpLabel":
            cut(cur_r, regions)
            cut(cur_b, blocks)
            continue
        if n in SKIP_IN_FUNC:
            continue
        if n in DEF_OPS:
            m.defs[i.tokens[1][0]] = i
        m.track(i)
        cur_r.append(i)
        cur_b.append(i)
        if n in TERMINATORS:
            cut(cur_r, regions)
            cut(cur_b, blocks)
        elif m.splits(i):
            cut(cur_r, regions)
    cut(cur_r, regions)
    cut(cur_b, blocks)

    out = {k: {} for k in LABELS}
    texts = {}
    for kind, groups in (("reg", regions), ("blk", blocks)):
        for insns in groups:
            for exact, label in ((False, "shape"), (True, "exact")):
                k, lines = m.key(insns, exact)
                d = out["%s_%s" % (kind, label)]
                e = d.get(k)
                if e is None:
                    d[k] = [1, len(insns)]
                else:
                    e[0] += 1
                if kind == "reg" and not exact and k not in texts:
                    texts[k] = " | ".join(lines[:3])[:240]

    fconst, words1 = set(), set()
    for rt, kind, w in m.consts.values():
        if kind != "L" or not w:
            continue
        words1.add(w[0])
        n_t, t_t = m.types.get(rt, ("?", []))
        if n_t == "OpTypeFloat" and t_t[0][0] == 32:
            fconst.add("%.6g" % struct.unpack("<f", struct.pack("<I", w[0]))[0])
    rng = ({RNG_CONSTS[x] for x in words1 if x in RNG_CONSTS} |
           {RNG_FLOAT_BITS[x] for x in words1 if x in RNG_FLOAT_BITS})

    layouts = set()
    for var, (sc, rt) in m.gvars.items():
        if sc in (2, 12) and DEC_BINDING in m.decor.get(var, {}):
            p = m.pointee(rt)
            if p is not None:
                layouts.add("%s %s" % (m.bindkey(var), m.layout(p)))

    out["texts"] = texts
    out["opcount"] = opcount
    out["sets"] = {
        "access": {"%s %s %s %s" % a for a in m.accesses},
        "bind": {a[0] for a in m.accesses},
        "layout": layouts,
        "fconst": fconst,
        "rng": rng,
        "ext": exts,
        "imgop": imgops,
    }
    out["insns"] = sum(1 for i in ins if i.name not in cd.DROP_OPS)
    return out


class Agg:
    def __init__(self):
        self.progs = collections.Counter()           # scope -> programs
        self.keyed = {}                              # (scope, label) -> [prog Counter, insn Counter, total]
        self.sets = collections.defaultdict(collections.Counter)   # (scope, set name) -> presence
        self.opcount = collections.defaultdict(collections.Counter)  # scope -> occurrences
        self.opprog = collections.defaultdict(collections.Counter)   # scope -> presence
        self.modsets = collections.defaultdict(collections.Counter)  # scope -> module-set proxy
        self.texts = {}
        self.nlen = {}                               # reg_shape key -> insns in the region
        self.reps = collections.defaultdict(list)    # kind -> (insns, hash, eidx)

    def add(self, kind, h, eidx, res):
        for scope in ("all", kind):
            self.progs[scope] += 1
            for label in LABELS:
                pc, ic, _ = self.keyed.setdefault((scope, label), [collections.Counter(), collections.Counter(), 0])
                tot = 0
                for k, (occ, nn) in res[label].items():
                    if label == "reg_shape":
                        self.nlen[k] = nn
                    pc[k] += 1
                    ic[k] += occ * nn
                    tot += occ * nn
                self.keyed[(scope, label)][2] += tot
            for name, vals in res["sets"].items():
                for v in vals:
                    self.sets[(scope, name)][v] += 1
            for n, c in res["opcount"].items():
                self.opcount[scope][n] += c
                self.opprog[scope][n] += 1
            ms = (tuple(sorted(res["sets"]["bind"])), tuple(sorted(res["sets"]["ext"])),
                  tuple(sorted(res["sets"]["imgop"])), tuple(sorted(res["sets"]["rng"])))
            self.modsets[scope][ms] += 1
        for k, v in res["texts"].items():
            self.texts.setdefault(k, v)
        self.reps[kind].append((res["insns"], h, eidx))


def coverage_line(pc, ic, total, nprog, label):
    order = sorted(pc, key=lambda k: (-pc[k], -ic[k]))
    cum = 0
    n50 = n90 = None
    for idx, k in enumerate(order):
        cum += ic[k]
        if n50 is None and cum >= 0.5 * total:
            n50 = idx + 1
        if n90 is None and cum >= 0.9 * total:
            n90 = idx + 1
            break
    def cov(n):
        return 100.0 * sum(ic[k] for k in order[:n]) / max(1, total)
    uniq = sum(ic[k] for k in pc if pc[k] == 1)
    print("  %-10s distinct=%-7d top20=%5.1f%% top50=%5.1f%% top100=%5.1f%% top1000=%5.1f%%  "
          "keys for 50%%=%s, 90%%=%s  unique-to-one-program=%5.1f%% (%.0f insn per program)"
          % (label, len(pc), cov(20), cov(50), cov(100), cov(1000), n50, n90,
             100.0 * uniq / max(1, total), uniq / max(1, nprog)))
    return order


def report(agg, top):
    print("programs: all %d, spawn %d, update %d" % (agg.progs["all"], agg.progs["spawn"], agg.progs["update"]))
    for scope in ("all", "spawn", "update"):
        print("\n== scope %s (%d programs) ==" % (scope, agg.progs[scope]))
        for label in LABELS:
            pc, ic, total = agg.keyed[(scope, label)]
            print("%s, function instructions covered: %d" % (label, total))
            order = coverage_line(pc, ic, total, agg.progs[scope], label)
            if scope == "all" and label == "reg_shape":
                print("  top %d shape regions (programs, insns, first lines):" % top)
                for k in order[:top]:
                    print("    %5d prog  %4d insn  %s" % (pc[k], agg.nlen.get(k, 0), agg.texts.get(k, "?")))
    print("\n== instruction mix (all programs): occurrences, programs ==")
    for n, c in agg.opcount["all"].most_common(45):
        print("  %-28s %9d %6d" % (n, c, agg.opprog["all"][n]))
    for name in ("access", "layout", "fconst", "rng", "ext", "imgop"):
        cnt = agg.sets[("all", name)]
        print("\n== %s: distinct %d (program counts) ==" % (name, len(cnt)))
        for v, c in cnt.most_common(top if name in ("access", "fconst") else 40):
            print("  %5d  %s" % (c, v))
    ms = agg.modsets["all"]
    print("\n== module-set proxy (bindings, GLSL/ext ops, image ops, RNG tags): distinct %d ==" % len(ms))
    for v, c in ms.most_common(12):
        print("  %5d  %s" % (c, repr(v)[:300]))
    for kind in ("spawn", "update"):
        reps = sorted(agg.reps[kind])
        if not reps:
            continue
        print("\n== %s insns: min %d, median %d, max %d ==" % (kind, reps[0][0], reps[len(reps) // 2][0],
                                                              reps[-1][0]))
        for insns, h, eidx in (reps[0], reps[len(reps) // 2], reps[-1]):
            print("  %016x emitter %d insns %d" % (h, eidx, insns))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", default=DEFAULT_PACK)
    ap.add_argument("--grammar", default=DEFAULT_GRAMMAR)
    ap.add_argument("--table", default=DEFAULT_TABLE)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--limit", type=int, default=0, help="analyse only the first N programs (testing)")
    ap.add_argument("--dump", action="append", default=[], help="program hash (hex) to write as .spv")
    ap.add_argument("--dump-dir")
    args = ap.parse_args()

    gr = cd.Grammar(args.grammar)
    names = cd.load_predict_names(args.table)
    targets = {}
    for h, ns in names.items():
        for nm in ns:
            mt = NAME_RE.match(nm)
            if mt:
                targets.setdefault(h, (mt.group(2), int(mt.group(1))))
    dumps = {int(x, 16) for x in args.dump}
    agg = Agg()
    with open(args.pack, "rb") as f:
        entries = cd.read_index(f)
        sel = [e for e in entries if e[0] in targets]
        print("pack variants %d, emitter spawn/update variants %d" % (len(entries), len(sel)))
        done = 0
        for e in sel:
            words = cd.read_spv(f, e)
            if cd.find_entry_model(words) != cd.GLCOMPUTE:
                continue
            ins = cd.parse_module(gr, words)
            kind, eidx = targets[e[0]]
            if e[0] in dumps and args.dump_dir:
                with open(os.path.join(args.dump_dir, "%016x.spv" % e[0]), "wb") as out:
                    out.write(words.tobytes())
            agg.add(kind, e[0], eidx, analyze(gr, ins))
            done += 1
            if args.limit and done >= args.limit:
                break
    report(agg, args.top)


if __name__ == "__main__":
    main()
