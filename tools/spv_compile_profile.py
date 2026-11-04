#!/usr/bin/env python3
# sotc: compile-cost profile of the compute shaders in a port shader cache (stdlib only).
"""Structural features of the compute SPIR-V in one shader cache folder, and what they cost in the driver.

Input:
  --cache DIR   a cache folder, user/cache/CUSA08809_v<hash>/. Each shader is 0x<pgm hash>_<perm>.spv
                (RegisterShaderBinary: name = {pgm_hash:#018x}_{perm}); its OpString holds {pgm_hash:#x}.
                Only compute modules are kept (OpEntryPoint execution model GLCompute). Without --cache,
                the newest user/cache/CUSA08809_v* folder that has .spv files is used.
  --tsv FILE    optional SOTC_PIPE_FEEDBACK_OUT file (kind, name, driver_ms, wall_ms, thread, hit).
                Its debug names are cs_0x<pgm hash> (GetShaderName), so each pipeline line maps to the
                .spv with that hash. A hash with several permutations uses the lowest one (the TSV does not
                say which permutation was built).

Output:
  1. distribution of every feature over the programs (and the shares of the big ones);
  2. with --tsv: Pearson correlation of each feature with driver ms, the slowest programs with their
     features, and a ridge least-squares fit (driver ms ~ features, stdlib Gauss elimination) that ranks
     features by the driver ms the fit attributes to them (coefficient times the feature's total over all
     pipelines in the TSV).

Features (per program, from one pass over the instructions):
  instructions, functions, blocks, branch_cond (OpBranchConditional), switch, loop_merge, phi, select,
  loads_<class> and stores_<class> for StorageBuffer, PhysicalStorageBuffer, Workgroup, Function, Image and
  other, atomics (OpAtomic*), image_ops (OpSampledImage, OpImage* ops), int64_ops (integer arithmetic,
  conversions and bitcasts whose result is a 64-bit int), access_chains (OpAccessChain, OpInBoundsAccessChain,
  OpPtrAccessChain, OpInBoundsPtrAccessChain), convert_u_to_ptr, calls (OpFunctionCall), ext_inst
  (OpExtInst, GLSL.std.450 and friends), max_nesting (depth of open OpSelectionMerge / OpLoopMerge
  constructs), max_block (most instructions in one basic block, label to terminator).
  The storage class of a load or store is that of its pointer operand (the class of the pointer type).
"""

import argparse
import glob
import math
import os
import re
import struct
import sys

MAGIC = 0x07230203
GLCOMPUTE = 5

# Opcodes (SPIR-V 1.x grammar).
OP_EXT_INST = 12
OP_NAME = 5
OP_ENTRY_POINT = 15
OP_TYPE_INT = 21
OP_TYPE_POINTER = 32
OP_FUNCTION = 54
OP_FUNCTION_CALL = 57
OP_FUNCTION_END = 56
OP_LOAD = 61
OP_STORE = 62
OP_ACCESS_CHAINS = {65, 66, 67, 70}
OP_CONVERT_U_TO_PTR = 120
OP_PHI = 245
OP_LOOP_MERGE = 246
OP_SELECTION_MERGE = 247
OP_LABEL = 248
OP_BRANCH_CONDITIONAL = 250
OP_SWITCH = 251
OP_SELECT = 169
TERMINATORS = {249, 250, 251, 252, 253, 254, 255}
ATOMIC_OPS = set(range(227, 243))          # OpAtomicLoad .. OpAtomicXor
IMAGE_OPS = set(range(86, 108))            # OpSampledImage, OpImageSample* .. OpImageQuerySamples
INT64_OPS = set(range(113, 115)) | {124} | set(range(126, 153)) | set(range(169, 206))
# Instructions without a result type (or with no result at all) among opcodes >= 41.
NO_TYPE_OPS = {62, 63, 64, 71, 72, 73, 74, 75, 99, 218, 219, 220, 221, 224, 225, 228,
               246, 247, 249, 250, 251, 252, 253, 254, 255, 256, 257, OP_LABEL}

STORAGE_CLASSES = {
    12: "StorageBuffer",
    5349: "PhysicalStorageBuffer",
    4: "Workgroup",
    7: "Function",
    11: "Image",
}
LOAD_CLASSES = ["StorageBuffer", "PhysicalStorageBuffer", "Workgroup", "Function", "Image", "other"]

COUNT_FEATURES = (
    ["instructions", "functions", "blocks", "branch_cond", "switch", "loop_merge", "phi", "select"]
    + ["loads_" + c for c in LOAD_CLASSES]
    + ["stores_" + c for c in LOAD_CLASSES]
    + ["atomics", "image_ops", "int64_ops", "access_chains", "convert_u_to_ptr", "calls", "ext_inst"]
)
SHAPE_FEATURES = ["max_nesting", "max_block"]
FEATURES = COUNT_FEATURES + SHAPE_FEATURES

HASH_NAME = re.compile(r"^0x([0-9a-f]+)_(\d+)\.spv$")
DEBUG_NAME = re.compile(r"^(\w+)_0x([0-9a-f]+)$")


def has_result_type(op):
    return op == OP_EXT_INST or (op >= 41 and op not in NO_TYPE_OPS and op != 73)


def class_name(storage_class):
    """Load/store bucket of a pointer's storage class (None when the pointer is unknown)."""
    return STORAGE_CLASSES.get(storage_class, "other")


def decode_literal(words):
    raw = struct.pack("<%dI" % len(words), *words)
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def analyze(path):
    """Returns (features dict, info dict) or raises ValueError for a module that is not SPIR-V."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 20 or len(data) % 4:
        raise ValueError("size")
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    if words[0] != MAGIC:
        raise ValueError("magic")

    feat = {name: 0 for name in FEATURES}
    int_width = {}      # type id -> width
    ptr_class = {}      # pointer type id -> storage class
    res_class = {}      # value id -> storage class (values of pointer type)
    names = {}          # id -> OpName string
    fn_size = {}        # function id -> instruction count
    fn_calls = {}       # callee id -> call count
    pgm_string = None
    compute = False

    in_fn = False
    cur_fn = None
    cur_fn_size = 0
    block_len = 0
    stack = []          # merge ids of the open structured constructs
    fn_depth = 0

    i, n = 5, len(words)
    while i < n:
        w0 = words[i]
        wc = w0 >> 16
        op = w0 & 0xFFFF
        if wc == 0 or i + wc > n:
            raise ValueError("truncated instruction at word %d" % i)
        ops = words[i + 1:i + wc]
        i += wc
        feat["instructions"] += 1

        if op == OP_NAME:
            names[ops[0]] = decode_literal(ops[1:])
            continue
        if op == 7 and pgm_string is None and len(ops) > 1:      # OpString
            text = decode_literal(ops[1:])
            if text.startswith("0x"):
                pgm_string = text
            continue
        if op == OP_ENTRY_POINT:
            if ops[0] == GLCOMPUTE:
                compute = True
            continue
        if op == OP_TYPE_INT:
            int_width[ops[0]] = ops[1]
            continue
        if op == OP_TYPE_POINTER:
            ptr_class[ops[0]] = ops[1]
            continue

        if op == OP_FUNCTION:
            in_fn = True
            cur_fn = ops[1]
            cur_fn_size = 0
            stack = []
            fn_depth = 0
            feat["functions"] += 1
            continue
        if op == OP_FUNCTION_END:
            if cur_fn is not None:
                fn_size[cur_fn] = cur_fn_size
            in_fn = False
            cur_fn = None
            feat["max_nesting"] = max(feat["max_nesting"], fn_depth)
            continue

        if has_result_type(op) and len(ops) >= 2:
            rt, rid = ops[0], ops[1]
            if rt in ptr_class:
                res_class[rid] = ptr_class[rt]
            if op in INT64_OPS and int_width.get(rt) == 64:
                feat["int64_ops"] += 1

        if not in_fn:
            continue

        # Inside a function body.
        if op == OP_LABEL:
            feat["blocks"] += 1
            block_len = 0
            lid = ops[0]
            while stack and stack[-1] == lid:
                stack.pop()
            continue

        cur_fn_size += 1
        block_len += 1
        if op in TERMINATORS:
            feat["max_block"] = max(feat["max_block"], block_len)

        if op == OP_LOAD:
            feat["loads_" + class_name(res_class.get(ops[2]))] += 1
        elif op == OP_STORE:
            feat["stores_" + class_name(res_class.get(ops[0]))] += 1
        elif op in ATOMIC_OPS:
            feat["atomics"] += 1
        elif op in IMAGE_OPS:
            feat["image_ops"] += 1
        elif op in OP_ACCESS_CHAINS:
            feat["access_chains"] += 1
        elif op == OP_CONVERT_U_TO_PTR:
            feat["convert_u_to_ptr"] += 1
        elif op == OP_FUNCTION_CALL:
            feat["calls"] += 1
            callee = ops[2]
            fn_calls[callee] = fn_calls.get(callee, 0) + 1
        elif op == OP_EXT_INST:
            feat["ext_inst"] += 1
        elif op == OP_BRANCH_CONDITIONAL:
            feat["branch_cond"] += 1
        elif op == OP_SWITCH:
            feat["switch"] += 1
        elif op == OP_LOOP_MERGE:
            feat["loop_merge"] += 1
            stack.append(ops[0])
            fn_depth = max(fn_depth, len(stack))
        elif op == OP_SELECTION_MERGE:
            stack.append(ops[0])
            fn_depth = max(fn_depth, len(stack))
        elif op == OP_PHI:
            feat["phi"] += 1
        elif op == OP_SELECT:
            feat["select"] += 1

    if not compute:
        return None, None

    biggest_fn = None
    if fn_size:
        biggest_fn = max(fn_size, key=lambda k: fn_size[k])
    most_called = None
    if fn_calls:
        most_called = max(fn_calls, key=lambda k: fn_calls[k])

    info = {
        "pgm_string": pgm_string,
        "largest_fn": (names.get(biggest_fn, "fn%d" % biggest_fn), fn_size[biggest_fn])
        if biggest_fn is not None else ("", 0),
        "most_called": (names.get(most_called, "fn%d" % most_called), fn_calls[most_called],
                        fn_size.get(most_called, 0))
        if most_called is not None else ("", 0, 0),
        "fn_names": names,
        "fn_calls": fn_calls,
        "fn_size": fn_size,
    }
    return feat, info


def scan_cache(folder):
    """Returns {pgm_hash_int: [(perm, path), ...]} for every .spv named like a pipeline shader."""
    table = {}
    for path in glob.glob(os.path.join(folder, "0x*.spv")):
        m = HASH_NAME.match(os.path.basename(path))
        if not m:
            continue
        table.setdefault(int(m.group(1), 16), []).append((int(m.group(2)), path))
    for lst in table.values():
        lst.sort()
    return table


def newest_cache(root):
    best = None
    for d in glob.glob(os.path.join(root, "CUSA08809_v*")):
        if not os.path.isdir(d):
            continue
        count = len(glob.glob(os.path.join(d, "0x*_*.spv")))
        if count == 0:
            continue
        mtime = os.path.getmtime(d)
        if best is None or mtime > best[0]:
            best = (mtime, d)
    return best[1] if best else None


def read_tsv(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline().rstrip("\n").split("\t")
        col = {name: idx for idx, name in enumerate(header)}
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < len(header):
                continue
            kind = parts[col["kind"]]
            if kind != "compute":
                continue
            rows.append({
                "name": parts[col["name"]],
                "driver_ms": float(parts[col["driver_ms"]]),
                "wall_ms": float(parts[col["wall_ms"]]),
                "thread": parts[col["thread"]] if "thread" in col else "",
                "hit": parts[col["hit"]] if "hit" in col else "0",
            })
    return rows


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return float("nan")
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx <= 0 or syy <= 0:
        return float("nan")
    return sxy / math.sqrt(sxx * syy)


def solve(a, b):
    """Gauss elimination with partial pivoting. a is k x k (list of lists), b length k."""
    k = len(b)
    m = [row[:] + [b[r]] for r, row in enumerate(a)]
    for c in range(k):
        piv = max(range(c, k), key=lambda r: abs(m[r][c]))
        if abs(m[piv][c]) < 1e-300:
            continue
        m[c], m[piv] = m[piv], m[c]
        for r in range(c + 1, k):
            f = m[r][c] / m[c][c]
            if f:
                for j in range(c, k + 1):
                    m[r][j] -= f * m[c][j]
    x = [0.0] * k
    for r in range(k - 1, -1, -1):
        s = m[r][k] - sum(m[r][j] * x[j] for j in range(r + 1, k))
        x[r] = s / m[r][r] if abs(m[r][r]) > 1e-300 else 0.0
    return x


def ridge_fit(rows, ys, lam):
    """rows: list of feature vectors (raw). Standardizes, fits ys ~ rows with ridge lam, returns
    (raw coefficients in ms per unit, intercept, R^2)."""
    n = len(rows)
    k = len(rows[0]) if rows else 0
    means = [sum(r[j] for r in rows) / n for j in range(k)]
    stds = []
    for j in range(k):
        var = sum((r[j] - means[j]) ** 2 for r in rows) / n
        stds.append(math.sqrt(var) if var > 0 else 1.0)
    my = sum(ys) / n
    z = [[(r[j] - means[j]) / stds[j] for j in range(k)] for r in rows]
    yc = [y - my for y in ys]
    a = [[0.0] * k for _ in range(k)]
    b = [0.0] * k
    for zr, yi in zip(z, yc):
        for p in range(k):
            zp = zr[p]
            b[p] += zp * yi
            ap = a[p]
            for q in range(p, k):
                ap[q] += zp * zr[q]
    for p in range(k):
        for q in range(p):
            a[p][q] = a[q][p]
        a[p][p] += lam * n
    beta = solve(a, b)
    raw = [beta[j] / stds[j] for j in range(k)]
    intercept = my - sum(raw[j] * means[j] for j in range(k))
    sst = sum(yc[i] ** 2 for i in range(n))
    sse = 0.0
    for r, y in zip(rows, ys):
        pred = intercept + sum(raw[j] * r[j] for j in range(k))
        sse += (y - pred) ** 2
    r2 = 1.0 - sse / sst if sst > 0 else float("nan")
    return raw, intercept, r2


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    idx = min(len(sorted_vals) - 1, int(p * len(sorted_vals)))
    return sorted_vals[idx]


def print_distribution(progs):
    print("Feature distribution over %d compute programs" % len(progs))
    print("%-22s %10s %10s %10s %10s %12s" % ("feature", "p50", "p90", "p99", "max", "total"))
    for name in FEATURES:
        vals = sorted(p[name] for p in progs)
        print("%-22s %10.0f %10.0f %10.0f %10.0f %12.0f" % (
            name, percentile(vals, 0.5), percentile(vals, 0.9), percentile(vals, 0.99), vals[-1],
            sum(vals)))
    total_instr = sum(p["instructions"] for p in progs) or 1
    print()
    print("Share of all compute instructions (%d):" % total_instr)
    for name in COUNT_FEATURES:
        if name == "instructions":
            continue
        total = sum(p[name] for p in progs)
        if total:
            print("  %-22s %10d  %6.2f%%" % (name, total, 100.0 * total / total_instr))
    print()


def print_largest(progs, count):
    print("Largest %d programs by instruction count" % count)
    print("%-20s %7s %6s %6s %7s %7s %6s %6s %6s %6s %6s %5s %5s  %s" % (
        "program", "instr", "blocks", "fns", "ldPSB", "stPSB", "ldSB", "stSB", "int64", "calls",
        "imgops", "nest", "blk", "biggest fn (name, instr) / most called (name, calls, instr)"))
    for p in sorted(progs, key=lambda p: -p["instructions"])[:count]:
        bf = p["largest_fn"]
        mc = p["most_called"]
        print("%-20s %7d %6d %6d %7d %7d %6d %6d %6d %6d %6d %5d %5d  %s (%d) / %s (%d x, %d)" % (
            p["program"], p["instructions"], p["blocks"], p["functions"],
            p["loads_PhysicalStorageBuffer"], p["stores_PhysicalStorageBuffer"],
            p["loads_StorageBuffer"], p["stores_StorageBuffer"], p["int64_ops"], p["calls"],
            p["image_ops"], p["max_nesting"], p["max_block"], bf[0], bf[1], mc[0], mc[1], mc[2]))
    print()


def print_correlations(rows):
    print("Correlation with driver ms (Pearson over %d pipelines)" % len(rows))
    ys = [r["driver_ms"] for r in rows]
    scored = []
    for name in FEATURES:
        xs = [r["feat"][name] for r in rows]
        scored.append((pearson(xs, ys), name))
    for c, name in sorted(scored, key=lambda t: 1.0 if math.isnan(t[0]) else -abs(t[0])):
        print("  %-22s r = %s" % (name, "   nan" if math.isnan(c) else "%+.3f" % c))
    print()


def print_slowest(rows, count):
    print("%d slowest compute pipelines (driver ms) with their features" % count)
    print("%9s %9s %7s %6s %6s %6s %6s %6s %6s %6s %5s %5s  %s" % (
        "driver", "wall", "instr", "blocks", "ldPSB", "stPSB", "ldSB", "stSB", "int64", "calls",
        "nest", "blk", "name"))
    for r in sorted(rows, key=lambda r: -r["driver_ms"])[:count]:
        f = r["feat"]
        print("%9.1f %9.1f %7d %6d %6d %6d %6d %6d %6d %6d %5d %5d  %s" % (
            r["driver_ms"], r["wall_ms"], f["instructions"], f["blocks"],
            f["loads_PhysicalStorageBuffer"], f["stores_PhysicalStorageBuffer"],
            f["loads_StorageBuffer"], f["stores_StorageBuffer"], f["int64_ops"], f["calls"],
            f["max_nesting"], f["max_block"], r["name"]))
    print()


def print_fit(rows, lam):
    ys = [r["driver_ms"] for r in rows]
    X = [[r["feat"][name] for name in FEATURES] for r in rows]
    raw, intercept, r2 = ridge_fit(X, ys, lam)
    total_ms = sum(ys)
    print("Ridge fit (lambda %.3g on standardized features): driver ms ~ features, R^2 = %.3f" % (lam, r2))
    print("intercept %.2f ms per pipeline; attributed ms = coefficient x total feature over all pipelines" % intercept)
    print("%-22s %14s %14s %12s %8s" % ("feature", "ms per unit", "attributed ms", "share", "total"))
    ranked = []
    for j, name in enumerate(FEATURES):
        total_feat = sum(x[j] for x in X)
        attributed = raw[j] * total_feat
        ranked.append((abs(attributed), name, raw[j], attributed, total_feat))
    for _, name, coef, attributed, total_feat in sorted(ranked, reverse=True):
        share = 100.0 * attributed / total_ms if total_ms else 0.0
        print("%-22s %14.6f %14.1f %11.2f%% %8.0f" % (name, coef, attributed, share, total_feat))
    print("total driver ms in the TSV: %.1f" % total_ms)
    print()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", help="cache folder (default: newest user/cache/CUSA08809_v* with .spv)")
    ap.add_argument("--cache-root", default=None,
                    help="user/cache directory used when --cache is not given")
    ap.add_argument("--tsv", help="SOTC_PIPE_FEEDBACK_OUT file")
    ap.add_argument("--top", type=int, default=30, help="slowest pipelines to list (with --tsv)")
    ap.add_argument("--largest", type=int, default=15, help="largest programs to list")
    ap.add_argument("--lam", type=float, default=1e-3, help="ridge penalty for the fit")
    args = ap.parse_args(argv)

    cache = args.cache
    if cache is None:
        root = args.cache_root or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                               "user", "cache")
        cache = newest_cache(root)
        if cache is None:
            print("no cache folder with .spv files under %s" % root, file=sys.stderr)
            return 1
    print("cache: %s" % cache)

    table = scan_cache(cache)
    progs = []
    by_hash = {}
    skipped = 0
    for pgm, lst in table.items():
        perm, path = lst[0]
        try:
            feat, info = analyze(path)
        except (ValueError, struct.error) as exc:
            skipped += 1
            print("skip %s: %s" % (path, exc), file=sys.stderr)
            continue
        if feat is None:
            continue
        if info["pgm_string"] and int(info["pgm_string"], 16) != pgm:
            print("warning: %s has OpString %s" % (path, info["pgm_string"]), file=sys.stderr)
        feat["program"] = "0x%x_%d" % (pgm, perm)
        feat["perms"] = len(lst)
        feat["largest_fn"] = info["largest_fn"]
        feat["most_called"] = info["most_called"]
        progs.append(feat)
        by_hash[pgm] = feat
    print("compute programs: %d (of %d pipeline hashes, %d non-compute or unreadable)" % (
        len(progs), len(table), len(table) - len(progs)))
    if skipped:
        print("unreadable: %d" % skipped)
    print()
    if not progs:
        return 1

    print_distribution(progs)
    print_largest(progs, args.largest)

    if args.tsv:
        rows = []
        missing = 0
        for r in read_tsv(args.tsv):
            m = DEBUG_NAME.match(r["name"])
            if not m or m.group(1) != "cs":
                missing += 1
                continue
            feat = by_hash.get(int(m.group(2), 16))
            if feat is None:
                missing += 1
                continue
            rows.append({"name": r["name"], "driver_ms": r["driver_ms"], "wall_ms": r["wall_ms"],
                         "feat": feat})
        print("TSV %s: %d compute pipelines matched, %d without a compute .spv in this cache" % (
            args.tsv, len(rows), missing))
        print()
        if len(rows) >= 3:
            print_correlations(rows)
            print_slowest(rows, args.top)
            print_fit(rows, args.lam)
        else:
            print("too few matched pipelines for correlations")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
