#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Classify the compute programs in a SOTC_PRETRANSLATE pack by SPIR-V similarity.

Input: sotc_pretranslate.pack (format in gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp):
  "SOTCPRE1", records (u64 hash, u32 perm, u32 meta bytes, u32 spv bytes, meta, spv),
  index at the end (u32 count, records (u64 hash, u32 perm, u64 offset, u32 meta, u32 spv)),
  u64 index offset, "SOTCPRE1".
Only the SPIR-V is used. A variant is a compute program when its OpEntryPoint execution model is GLCompute (5).

Normalisation per variant (the SPIR-V grammar drives which operand words are ids):
  - drop debug and source instructions (OpSource*, OpName, OpMemberName, OpString, OpLine, OpNoLine,
    OpModuleProcessed); the header (magic, version, generator, bound, schema) is not hashed;
  - renumber ids in order of first appearance, so the bound and the generator's numbering do not matter.
Three keys per variant:
  full    the normalised stream; equal keys = identical programs;
  const   as full, but the literal words of OpConstant/OpSpecConstant values, of OpDecorate/OpMemberDecorate
          parameters, and of OpExecutionMode parameters (LocalSize) are zeroed; equal keys = same shape,
          constants may differ;
  skel    every literal word zeroed; equal keys = same opcode and id structure.

Output: a summary on stdout. --tsv writes one row per compute variant (keep it outside the repo).
--samples N prints, for the variants whose "const" key matches but "full" differs, which constants differ and
their context (type, first user opcode, decoration name).

usage:
  python -I tools/re/compute_dupes.py [--pack PACK] [--grammar GRAMMAR] [--predict TSV] [--tsv OUT]
                                       [--samples N] [--max-groups N]
  Run with -I when the pack or the grammar sits in a shared folder (the scripts do not import anything but stdlib).
"""
import argparse
import array
import collections
import hashlib
import json
import os
import struct
import sys

DEFAULT_PACK = "user/cache/CUSA08809_v13272bcc/sotc_pretranslate.pack"
DEFAULT_GRAMMAR = "C:/msys64/clang64/include/spirv/unified1/spirv.core.grammar.json"
DEFAULT_PREDICT = "user/shader_predict/predict_table.tsv"
PACK_MAGIC = b"SOTCPRE1"
GLCOMPUTE = 5

ID_KINDS = {"IdResultType", "IdResult", "IdRef", "IdMemorySemantics", "IdScope"}
DROP_OPS = {"OpSourceContinued", "OpSource", "OpSourceExtension", "OpName", "OpMemberName", "OpString",
            "OpLine", "OpNoLine", "OpModuleProcessed"}
# Literal operand indices (after the opcode word) that the "const" key zeroes.
CONST_MASK_FROM = {"OpConstant": 0, "OpSpecConstant": 0, "OpDecorate": 2, "OpMemberDecorate": 3,
                   "OpExecutionMode": 2, "OpExecutionModeId": 2}


class Grammar:
    def __init__(self, path):
        with open(path, encoding="utf-8") as f:
            g = json.load(f)
        self.ops = {}
        for ins in g["instructions"]:
            self.ops[ins["opcode"]] = (ins["opname"], [(o["kind"], o.get("quantifier", "")) for o in
                                                       ins.get("operands", [])])
        self.kinds = {k["kind"]: k for k in g["operand_kinds"]}
        # Value enums: value -> parameter plan. Bit enums: ordered (bit, plan) list.
        self.value_params = {}
        self.bit_params = {}
        for k in g["operand_kinds"]:
            if k.get("category") == "ValueEnum":
                self.value_params[k["kind"]] = {e["value"]: [(p["kind"], p.get("quantifier", ""))
                                                             for p in e.get("parameters", [])]
                                                for e in k.get("enumerants", [])}
            elif k.get("category") == "BitEnum":
                lst = []
                for e in k.get("enumerants", []):
                    v = int(e["value"], 0) if isinstance(e["value"], str) else e["value"]
                    if v == 0 and not e.get("parameters"):
                        continue
                    lst.append((v, [(p["kind"], p.get("quantifier", "")) for p in e.get("parameters", [])]))
                self.bit_params[k["kind"]] = sorted(lst)
        self.opnames = {op: name for op, (name, _) in self.ops.items()}
        self.enum_names = {k["kind"]: {e["value"]: e["enumerant"] for e in k.get("enumerants", [])}
                           for k in g["operand_kinds"] if k.get("category") in ("ValueEnum", "BitEnum")}


class DecodeError(Exception):
    pass


def _is_zero_byte_word(w):
    return (w & 0xFF) == 0 or (w & 0xFF00) == 0 or (w & 0xFF0000) == 0 or (w & 0xFF000000) == 0


def parse_operands(gr, plan, words, pos, end, tokens):
    """Consume operand words [pos, end) according to plan; append (word_index, is_id) to tokens."""
    for kind, quant in plan:
        if quant == "*":
            while pos < end:
                npos = _parse_one(gr, kind, words, pos, end, tokens)
                if npos == pos:
                    break
                pos = npos
        elif quant == "?":
            if pos < end:
                pos = _parse_one(gr, kind, words, pos, end, tokens)
        else:
            pos = _parse_one(gr, kind, words, pos, end, tokens)
    return pos


def _parse_one(gr, kind, words, pos, end, tokens):
    if pos >= end:
        return pos
    if kind in ID_KINDS:
        tokens.append((pos, True))
        return pos + 1
    if kind == "LiteralString":
        while pos < end:
            tokens.append((pos, False))
            w = words[pos]
            pos += 1
            if _is_zero_byte_word(w):
                break
        return pos
    if kind == "LiteralContextDependentNumber":
        # Always the last operand of OpConstant/OpSpecConstant: the rest of the instruction.
        while pos < end:
            tokens.append((pos, False))
            pos += 1
        return pos
    if kind in gr.value_params:
        tokens.append((pos, False))
        params = gr.value_params[kind].get(words[pos], [])
        return parse_operands(gr, params, words, pos + 1, end, tokens)
    if kind in gr.bit_params:
        mask = words[pos]
        tokens.append((pos, False))
        pos += 1
        for bit, params in gr.bit_params[kind]:
            if mask & bit:
                pos = parse_operands(gr, params, words, pos, end, tokens)
        return pos
    spec = gr.kinds.get(kind)
    if spec is not None and "bases" in spec:
        return parse_operands(gr, [(b, "") for b in spec["bases"]], words, pos, end, tokens)
    # Literal kinds (LiteralInteger, LiteralFloat, LiteralExtInstInteger, LiteralSpecConstantOpInteger, ...).
    tokens.append((pos, False))
    return pos + 1


class Insn:
    __slots__ = ("op", "name", "wc", "tokens", "words")

    def __init__(self, op, name, wc, tokens, words):
        self.op = op
        self.name = name
        self.wc = wc
        self.tokens = tokens  # list of (value, is_id) in operand order (after the opcode word)
        self.words = words


def find_entry_model(words):
    """Execution model of the first OpEntryPoint, or None. Walks word headers only."""
    i = 5
    n = len(words)
    while i < n:
        w0 = words[i]
        wc = w0 >> 16
        op = w0 & 0xFFFF
        if wc == 0:
            return None
        if op == 15:
            return words[i + 1]
        i += wc
    return None


def parse_module(gr, words):
    insns = []
    i = 5
    n = len(words)
    while i < n:
        w0 = words[i]
        wc = w0 >> 16
        op = w0 & 0xFFFF
        if wc == 0 or i + wc > n:
            raise DecodeError("bad word count at %d" % i)
        name, plan = gr.ops.get(op, ("Op?%d" % op, None))
        tokens = []
        if plan is None:
            # Unknown opcode: its operands are taken as literals (no ids renumbered).
            for j in range(i + 1, i + wc):
                tokens.append((j, False))
        else:
            end = i + wc
            pos = parse_operands(gr, plan, words, i + 1, end, tokens)
            if pos != end:
                raise DecodeError("operand mismatch in %s at %d" % (name, i))
        insns.append(Insn(op, name, wc, [(words[p], isid) for p, isid in tokens],
                          [words[j] for j in range(i + 1, i + wc)]))
        i += wc
    return insns


def normalise(insns, mode):
    """mode: 'full', 'const' or 'skel'. Returns the normalised stream as array('I')."""
    ids = {}
    out = array.array("I")
    for ins in insns:
        if ins.name in DROP_OPS:
            continue
        out.append(ins.op | (ins.wc << 16))
        mask_from = None
        if mode == "const":
            mask_from = CONST_MASK_FROM.get(ins.name)
        for idx, (val, isid) in enumerate(ins.tokens):
            if isid:
                m = ids.get(val)
                if m is None:
                    m = len(ids) + 1
                    ids[val] = m
                out.append(m)
            elif mode == "skel" or (mask_from is not None and idx >= mask_from):
                out.append(0)
            else:
                out.append(val)
    return out


def digest(arr):
    return hashlib.sha1(arr.tobytes()).hexdigest()[:16]


def read_index(f):
    f.seek(-16, os.SEEK_END)
    index_offset, = struct.unpack("<Q", f.read(8))
    if f.read(8) != PACK_MAGIC:
        raise SystemExit("not a SOTCPRE1 pack")
    f.seek(index_offset)
    count, = struct.unpack("<I", f.read(4))
    entries = []
    for _ in range(count):
        hsh, perm, offset, meta_size, spv_size = struct.unpack("<QIQII", f.read(28))
        entries.append((hsh, perm, offset, meta_size, spv_size))
    return entries


def read_spv(f, entry):
    hsh, perm, offset, meta_size, spv_size = entry
    f.seek(offset + meta_size)
    raw = f.read(spv_size)
    words = array.array("I")
    words.frombytes(raw)
    if sys.byteorder == "big":
        words.byteswap()
    return words


def load_predict_names(path):
    names = {}
    if not path or not os.path.exists(path):
        return names
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3 and parts[1] == "4":
                names.setdefault(int(parts[0], 16), set()).add(parts[2])
    return names


def const_sites(gr, insns):
    """Label every masked literal word of a module: (insn index, token index) -> context label."""
    typ = {}
    const_type = {}
    for ins in insns:
        if ins.name.startswith("OpType") and ins.tokens:
            if ins.name in ("OpTypeFloat", "OpTypeInt") and len(ins.tokens) > 1:
                typ[ins.tokens[0][0]] = "%s%d" % (ins.name[6:], ins.tokens[1][0])
            else:
                typ[ins.tokens[0][0]] = ins.name[6:]
        if ins.name in ("OpConstant", "OpSpecConstant") and len(ins.tokens) >= 2:
            const_type[ins.tokens[1][0]] = ins.tokens[0][0]
    first_user = {}
    for ins in insns:
        if ins.name in ("OpDecorate", "OpMemberDecorate", "OpExecutionMode", "OpName"):
            continue
        for val, isid in ins.tokens[2:] if ins.name in ("OpConstant", "OpSpecConstant") else ins.tokens:
            if isid and val in const_type and val not in first_user:
                first_user[val] = ins.name
    labels = {}
    for i, ins in enumerate(insns):
        mask_from = CONST_MASK_FROM.get(ins.name)
        if mask_from is None:
            continue
        for k, (val, isid) in enumerate(ins.tokens):
            if isid or k < mask_from:
                continue
            if ins.name in ("OpConstant", "OpSpecConstant"):
                t = const_type.get(ins.tokens[1][0])
                label = "%s constant, used by %s" % (typ.get(t, "?"), first_user.get(ins.tokens[1][0], "nothing"))
            elif ins.name == "OpDecorate" and k == 2 or ins.name == "OpMemberDecorate" and k >= 3:
                dec = ins.tokens[1][0] if ins.name == "OpDecorate" else ins.tokens[2][0]
                label = "decoration %s" % gr.enum_names["Decoration"].get(dec, dec)
            elif ins.name in ("OpDecorate", "OpMemberDecorate"):
                dec = ins.tokens[1][0] if ins.name == "OpDecorate" else ins.tokens[2][0]
                label = "decoration %s parameter" % gr.enum_names["Decoration"].get(dec, dec)
            else:
                mode = ins.tokens[1][0]
                label = "execution mode %s" % gr.enum_names["ExecutionMode"].get(mode, mode)
            labels[(i, k)] = label
    return labels


def explain_const_groups(gr, f, rows, const_g, full_g, samples):
    """For const groups holding several full classes: which constant words differ, and their context."""
    shown = 0
    for key, members in sorted(const_g.items(), key=lambda kv: -len({m["full"] for m in kv[1]})):
        classes = {}
        for m in members:
            classes.setdefault(m["full"], m)
        if len(classes) < 2:
            continue
        shown += 1
        if shown > samples:
            break
        a, b = list(classes.values())[:2]
        ins_a = parse_module(gr, read_spv(f, a["_entry"]))
        ins_b = parse_module(gr, read_spv(f, b["_entry"]))
        labels = const_sites(gr, ins_a)
        diffs = collections.Counter()
        examples = {}
        for pos, label in labels.items():
            va = ins_a[pos[0]].tokens[pos[1]][0]
            vb = ins_b[pos[0]].tokens[pos[1]][0]
            if va != vb:
                diffs[label] += 1
                examples.setdefault(label, (va, vb))
        print("const group x%d (%d full classes, insn %d): %016x perm %d vs %016x perm %d"
              % (len(members), len(classes), a["insns"], a["hash"], a["perm"], b["hash"], b["perm"]))
        for label, n in diffs.most_common():
            va, vb = examples[label]
            print("    %4d literal(s) differ: %-55s e.g. %s vs %s" % (n, label, _fmt(va), _fmt(vb)))


def _fmt(v):
    if 0 < v < 0x100000 or v == 0:
        return str(v)
    f = struct.unpack("<f", struct.pack("<I", v))[0]
    return "%d (float %g)" % (v, f)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", default=DEFAULT_PACK)
    ap.add_argument("--grammar", default=DEFAULT_GRAMMAR)
    ap.add_argument("--predict", default=DEFAULT_PREDICT)
    ap.add_argument("--tsv", help="write one row per compute variant here")
    ap.add_argument("--samples", type=int, default=3, help="const-variant groups to explain in detail")
    ap.add_argument("--max-groups", type=int, default=10)
    args = ap.parse_args()

    gr = Grammar(args.grammar)
    names = load_predict_names(args.predict)
    rows = []
    stage_count = collections.Counter()
    with open(args.pack, "rb") as f:
        entries = read_index(f)
        print("pack variants: %d" % len(entries))
        compute = []
        for e in entries:
            words = read_spv(f, e)
            model = find_entry_model(words)
            stage_count[model] += 1
            if model == GLCOMPUTE:
                compute.append(e)
        print("compute variants: %d (programs: %d)" % (len(compute), len({e[0] for e in compute})))
        print("entry execution models of all variants:", dict(stage_count))

        # Pass 1: keys for every compute variant.
        for e in compute:
            words = read_spv(f, e)
            insns = parse_module(gr, words)
            full = normalise(insns, "full")
            const = normalise(insns, "const")
            skel = normalise(insns, "skel")
            rows.append({
                "_entry": e, "hash": e[0], "perm": e[1], "insns": sum(1 for i in insns if i.name not in DROP_OPS),
                "words": len(words), "full": digest(full), "const": digest(const), "skel": digest(skel),
                "names": "|".join(sorted(names.get(e[0], []))),
            })

    # Summaries.
    def group(key):
        g = collections.defaultdict(list)
        for r in rows:
            g[r[key]].append(r)
        return g

    full_g = group("full")
    const_g = group("const")
    skel_g = group("skel")
    progs = {r["hash"] for r in rows}
    print("distinct programs among compute variants: %d" % len(progs))
    print("distinct full keys (identical-program classes): %d" % len(full_g))
    print("distinct const keys (same shape, constants may differ): %d" % len(const_g))
    print("distinct skeleton keys (same opcode/id structure): %d" % len(skel_g))

    def describe(gmap, label):
        sizes = sorted((len(v) for v in gmap.values()), reverse=True)
        singles = sum(1 for s in sizes if s == 1)
        print("%s: groups=%d singletons=%d largest=%s" % (label, len(sizes), singles, sizes[:10]))
        multi = sorted(((len(v), k) for k, v in gmap.items() if len(v) > 1), reverse=True)
        return multi

    print()
    multi_full = describe(full_g, "full groups")
    print("  variants in groups of 2+: %d of %d" % (sum(n for n, _ in multi_full), len(rows)))
    multi_const = describe(const_g, "const groups")
    describe(skel_g, "skeleton groups")

    # Instruction weighting: count of unique full classes and their instruction totals.
    total_insn = sum(r["insns"] for r in rows)
    uniq_insn = sum(v[0]["insns"] for v in full_g.values())
    print()
    print("instructions, all variants: %d; with one copy per identical class: %d (%.1f%%)" %
          (total_insn, uniq_insn, 100.0 * uniq_insn / max(1, total_insn)))
    # Cost model from the brief: 4.7 ms per pipeline + 0.04 ms per instruction.
    cost_all = len(rows) * 4.7 + total_insn * 0.04
    cost_uniq = len(full_g) * 4.7 + uniq_insn * 0.04
    print("model cost (4.7 ms/pipeline + 0.04 ms/insn): all %.0f s, one per identical class %.0f s"
          % (cost_all / 1000, cost_uniq / 1000))
    cost_const = len(const_g) * 4.7 + sum(v[0]["insns"] for v in const_g.values()) * 0.04
    print("model cost if one pipeline per const class (constants made runtime data): %.0f s" % (cost_const / 1000))

    print()
    print("largest identical-program groups (variants, instructions, example program hashes):")
    for n, k in multi_full[:args.max_groups]:
        members = full_g[k]
        print("  x%-4d insn=%-6d progs=%-4d %s" % (n, members[0]["insns"], len({m["hash"] for m in members}),
                                               " ".join("%016x" % m["hash"] for m in members[:3])))
    print()
    print("largest const-shape groups (variants, distinct full keys inside):")
    for n, k in multi_const[:args.max_groups]:
        members = const_g[k]
        print("  x%-4d distinct_full=%-4d insn=%d" % (n, len({m["full"] for m in members}), members[0]["insns"]))

    if args.samples:
        print()
        with open(args.pack, "rb") as f:
            explain_const_groups(gr, f, rows, const_g, full_g, args.samples)

    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8") as out:
            out.write("hash\tperm\tinsns\twords\tfull\tconst\tskel\tnames\n")
            for r in rows:
                out.write("%016x\t%d\t%d\t%d\t%s\t%s\t%s\t%s\n" % (r["hash"], r["perm"], r["insns"], r["words"],
                                                             r["full"], r["const"], r["skel"], r["names"]))
        print("wrote", args.tsv)


if __name__ == "__main__":
    main()
