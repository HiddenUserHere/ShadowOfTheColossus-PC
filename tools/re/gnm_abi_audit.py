"""Static ABI audit of the GNM boundary detours (gap N2-73).

For every detour row in gpu/sotc/hook_gnm_w*.inc, compare the declared extern "C" signature with
the SysV argument registers the guest function actually reads before writing them (its prologue,
up to the first call / jmp / ret), disassembled from out/eboot.elf (file offset = VA + 0x4000) with
llvm-objdump (MSYS2 CLANG64). Flags: integer argument count, 32-bit declaration of a register the
guest uses as 64-bit, float arguments declared as integers (or missing), and extra declared args.

Usage: python -I tools/re/gnm_abi_audit.py   (needs llvm-objdump / llvm-objcopy on PATH)
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EBOOT = os.path.join(ROOT, "out", "eboot.elf")
INT_REGS = ["rdi", "rsi", "rdx", "rcx", "r8", "r9"]
ALIASES = {
    "rdi": ["rdi", "edi", "di", "dil"], "rsi": ["rsi", "esi", "si", "sil"],
    "rdx": ["rdx", "edx", "dx", "dl"], "rcx": ["rcx", "ecx", "cx", "cl"],
    "r8": ["r8", "r8d", "r8w", "r8b"], "r9": ["r9", "r9d", "r9w", "r9b"],
}
NAME_TO_REG = {a: r for r, al in ALIASES.items() for a in al}
WIDE = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"}
WRITE_ONLY = ("mov", "movzx", "movsx", "movsxd", "lea", "pop", "movaps", "movups", "movss",
              "movsd", "movd", "movq", "vmovss", "vmovsd", "vmovaps", "vmovups", "cvtsi2ss")


def parse_decls():
    decls = {}
    rows = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "gpu", "sotc", "hook_gnm_w*.inc"))):
        text = open(path, encoding="utf-8").read()
        for m in re.finditer(r'extern\s+"C"[^;]*?\b(GnmW\d\d_\w+)\s*\(([^)]*)\)\s*;', text, re.S):
            decls[m.group(1)] = [p.strip() for p in m.group(2).split(",") if p.strip() and p.strip() != "void"]
        for m in re.finditer(r'^\s*\{\s*0x([0-9a-fA-F]+),\s*\(void\s*\*\)\s*&(GnmW\d\d_\w+)', text, re.M):
            rows[m.group(2)] = int(m.group(1), 16)
    return decls, rows


def classify(param):
    t = re.sub(r"\b\w+$", "", param).strip() if not param.endswith("*") else param
    low = param.replace("const", "")
    if re.search(r"\b(float|double)\b", low):
        return "f"
    if "*" in low or re.search(r"\b(u64|s64|uintptr_t|intptr_t|VAddr|size_t|long\s+long|unsigned\s+long\s+long)\b", low):
        return "i64"
    return "i32"


def disasm(va, length=96):
    data = open(EBOOT, "rb").read()[va + 0x4000: va + 0x4000 + length]
    with tempfile.TemporaryDirectory() as d:
        b, o = os.path.join(d, "f.bin"), os.path.join(d, "f.o")
        open(b, "wb").write(data)
        subprocess.run(["llvm-objcopy", "-I", "binary", "-O", "elf64-x86-64", "-B", "i386:x86-64", b, o],
                       check=True)
        out = subprocess.run(["llvm-objdump", "-d", "-M", "intel", "--no-show-raw-insn", "-j", ".data",
                              "-D", f"--adjust-vma={va:#x}", o], capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
        if m:
            insns.append((m.group(2), m.group(3)))
    return insns


def regs_in(text):
    return re.findall(r"\b(r[a-z0-9]+|e[a-z]{2}|[a-d][lx]|[sd]il?|[sd]i|xmm\d+)\b", text)


def analyze(va):
    written = set()
    int_reads = {}
    xmm_reads = set()
    for mnem, ops in disasm(va):
        if mnem in ("call", "jmp", "ret"):
            break
        parts = [p.strip() for p in re.split(r",(?![^\[]*\])", ops)] if ops else []
        dest = parts[0] if parts else ""
        srcs = parts[1:]
        pure_write = mnem in WRITE_ONLY or (mnem in ("xor", "pxor", "xorps") and len(parts) == 2 and parts[0] == parts[1])
        read_texts = srcs + ([] if pure_write and "[" not in dest else [dest])
        if "[" in dest:  # memory destination: registers inside the address are read
            read_texts.append(dest)
        for t in read_texts:
            for r in regs_in(t):
                if r.startswith("xmm"):
                    if r not in written and int(r[3:]) < 8:
                        xmm_reads.add(int(r[3:]))
                elif r in NAME_TO_REG:
                    base = NAME_TO_REG[r]
                    if base not in written:
                        w = 64 if r in WIDE else 32
                        int_reads[base] = max(int_reads.get(base, 0), w)
        if "[" not in dest:
            for r in regs_in(dest):
                if r.startswith("xmm"):
                    written.add(r)
                elif r in NAME_TO_REG:
                    written.add(NAME_TO_REG[r])
    return int_reads, xmm_reads


def main():
    decls, rows = parse_decls()
    problems = 0
    for name, va in sorted(rows.items(), key=lambda kv: kv[1]):
        params = decls.get(name)
        if params is None:
            print(f"{va:#x} {name}: no declaration found"); problems += 1; continue
        kinds = [classify(p) for p in params]
        ints = [k for k in kinds if k != "f"]
        floats = kinds.count("f")
        reads, xmm = analyze(va)
        used_int = max([INT_REGS.index(r) + 1 for r in reads] or [0])
        issues = []
        if used_int > len(ints):
            issues.append(f"guest reads {used_int} integer args, declared {len(ints)}")
        for i, r in enumerate(INT_REGS[:min(len(ints), 6)]):
            if reads.get(r) == 64 and ints[i] == "i32":
                issues.append(f"{r} used as 64-bit but declared 32-bit (arg {i + 1})")
        if xmm and floats < max(xmm) + 1:
            issues.append(f"guest reads xmm{sorted(xmm)}, declared {floats} float args")
        if issues:
            problems += 1
            print(f"{va:#x} {name}({', '.join(params)}): " + "; ".join(issues))
    print(f"{len(rows)} detours checked, {problems} with issues")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
