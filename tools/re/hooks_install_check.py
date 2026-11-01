#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline port of the detour engine's acceptance rules (gpu/sotc/cpu_patches.cpp
InstallGuestDetour and the analysis it calls), for every hook of scripts/hooks.d.

tools/re/hooks_check.py only proves that a signature resolves to one site. This script also
predicts whether the loader will accept the detour for that site: it runs the same refusal
checks, in the same order, with the same reason strings, on the eboot's bytes. Declarations are
loaded the way hooks_check.py loads them, and resolution (expected offset or unique scan) is the
same, so a site that fails to resolve is reported with hooks_check's reason.

    python tools/re/hooks_install_check.py                  out/eboot.elf, every declared hook
    python tools/re/hooks_install_check.py --eboot PATH     an ELF, an eboot.bin or a game folder
    python tools/re/hooks_install_check.py --allow-refused  report only, exit 0
    python tools/re/hooks_install_check.py --validate       the built-in validation table (see below)
    python tools/re/hooks_install_check.py --sse4a --tsc-qpc
                                                            also model the host-dependent CPU passes

Output: one line per hook, "SOTC DETOUR <name> 0x<site> accepted|refused mode=<mode> ...", then
the summary. A refused hook gets the reason the engine would log (InstallGuestDetour's reason,
without the "detour: " prefix of the manifest).

Modelled (each rule names its place in gpu/sotc/cpu_patches.cpp):
- resolution: hooks_check.resolve (expected offset, else unique scan, anchor function, range length);
- InstallGuestDetour, in order: module bounds, pattern length, function table, entry rule (not for
  site), red-zone protected functions, DecodeFunction (indirect branches, jump tables), stack
  arguments (ret imm, Wrap/Time), CPU patches of earlier passes, a branch to the entry, the displaced
  span (NearJumpSize = 5 bytes, whole instructions, branch targets, decoded bytes, earlier patches),
  control flow inside the span, the function end, Time's tail-call rule, WrapStackRefusal,
  relocation of the displaced instructions;
- dependencies, anchors and groups as the loader's Propagate().

Not modelled exactly (see the report of the port): the earlier CPU passes are modelled as their
effect on bytes and ranges (red-zone functions of sotc_cpu.py as whole protected functions;
SSE4a and rdtsc only with --sse4a / --tsc-qpc, as the patched instructions); the trampoline size
and rel32 range checks (the trampoline is 16 MiB and always in range); time thunks; the
interplay of installed detours of other hooks (each hook is checked on its own, as if alone);
and the switches (every declared hook is checked, whether its switch is set or not).
"""
import argparse
import bisect
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools' / 're'))

import capstone  # noqa: E402
import hooks_check  # noqa: E402
import sotc_cpu  # noqa: E402  (the red-zone functions gpu/sotc protects at load)
import sotc_hooks  # noqa: E402

NEAR_JUMP_SIZE = 5          # cpu_patches.cpp: constexpr size_t NearJumpSize = 5
TIME_SLOT_MARGIN = 8        # WrapStackRefusal: kTimeSlotMargin
MAX_JUMP_TABLE = 4096       # ResolveBoundedJumpTable: MaxJumpTableEntries
MAX_TABLE_SCAN = 64         # MaxPatternInstructions (both scans)
MAX_INTERVENING = 4         # MaxInterveningInstructions

# Category of a decoded instruction, as Zydis' meta.category is used by the engine.
CALL, COND, UNCOND, RET, INTERRUPT, SYSRET, OTHER = 'call', 'cond', 'uncond', 'ret', 'int', 'sysret', 'other'

NOT_STACK = object()        # rbp holds no copy of rsp (kNotStack)
# An unknown stack depth or rbp value is None (kUnknown).

WRITE = 2                   # capstone CS_AC_WRITE
PUSH_MNEMONICS = {'push', 'pushf', 'pushfd', 'pushfq', 'pushw'}
POP_MNEMONICS = {'pop', 'popf', 'popfd', 'popfq', 'popw'}
IMPLICIT_SP = PUSH_MNEMONICS | POP_MNEMONICS | {'leave', 'enter'}
RELOC_RIP_NAMES = {'rip', 'eip'}

_GPR_ALIASES = {}
for _base, _names in {
        'rax': ('eax', 'ax', 'al', 'ah'), 'rbx': ('ebx', 'bx', 'bl', 'bh'),
        'rcx': ('ecx', 'cx', 'cl', 'ch'), 'rdx': ('edx', 'dx', 'dl', 'dh'),
        'rsi': ('esi', 'si', 'sil'), 'rdi': ('edi', 'di', 'dil'),
        'rbp': ('ebp', 'bp', 'bpl'), 'rsp': ('esp', 'sp', 'spl')}.items():
    for _alias in _names:
        _GPR_ALIASES[_alias] = _base
for _n in range(8, 16):
    for _suffix in ('d', 'w', 'b'):
        _GPR_ALIASES[f'r{_n}{_suffix}'] = f'r{_n}'


def parent64(name):
    """The 64-bit register of a register name (ZydisRegisterGetLargestEnclosing for x86-64)."""
    return _GPR_ALIASES.get(name, name)


def is_sp(name):
    return bool(name) and parent64(name) == 'rsp'


def is_bp(name):
    return bool(name) and parent64(name) == 'rbp'


def s32_fix(value):
    """Capstone reports a sign-extended imm32/imm8 of a 64-bit operation zero-extended from 32
    bits (and rsp, -16 is 0xfffffff0); the engine's Zydis operands are signed."""
    if 0x80000000 <= value < 0x100000000:
        return value - 0x100000000
    return value


class Op:
    __slots__ = ('kind', 'name', 'base', 'index', 'scale', 'disp', 'imm', 'access')

    def __init__(self, kind, name='', base='', index='', scale=0, disp=0, imm=0, access=0):
        self.kind, self.name, self.base, self.index = kind, name, base, index
        self.scale, self.disp, self.imm, self.access = scale, disp, imm, access


class Ins:
    """One decoded instruction: the fields of the engine's DecodedCodeInstruction that the
    acceptance rules read. ops are the explicit (visible) operands, as Zydis' operand_count_visible."""
    __slots__ = ('address', 'length', 'mnemonic', 'category', 'ops', 'writes', 'changes_sp',
                 'sp_delta', 'target')

    def is_terminator(self):
        return self.category in (UNCOND, RET, INTERRUPT, SYSRET) or self.mnemonic == 'ud2'

    def is_detour_control_flow(self):
        return self.category in (CALL, COND) or self.is_terminator()


def _category(mnemonic):
    if mnemonic == 'call':
        return CALL
    if mnemonic == 'jmp':
        return UNCOND
    if mnemonic.startswith('ret'):
        return RET
    if mnemonic.startswith('j') or mnemonic in ('loop', 'loope', 'loopne', 'loopz', 'loopnz'):
        return COND
    if mnemonic in ('int', 'int1', 'int3', 'into') or mnemonic.startswith('iret'):
        return INTERRUPT
    if mnemonic.startswith('sysret') or mnemonic.startswith('sysexit'):
        return SYSRET
    return OTHER


class Decoder:
    """Capstone decoding of the image, cached by address. A decode never reads past the end the
    caller gives (Zydis is called with end - address)."""

    def __init__(self, image):
        self.image = image
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.cache = {}

    def _reg(self, reg):
        name = self.md.reg_name(reg) if reg else ''
        return name or ''

    def at(self, address, end):
        """The instruction at address, or None when it does not decode inside [address, end)."""
        if address in self.cache:
            ins = self.cache[address]
        else:
            ins = self._decode(address)
            self.cache[address] = ins
        if ins is None or ins.length > end - address:
            return None
        return ins

    def _decode(self, address):
        code = self.image.read(address, 16)
        if not code:
            return None
        found = next(self.md.disasm(code, address, 1), None)
        if found is None:
            return None
        ins = Ins()
        ins.address, ins.length, ins.mnemonic = address, found.size, found.mnemonic
        ins.category = _category(found.mnemonic)
        ins.ops = []
        for op in found.operands:
            if op.type == capstone.x86.X86_OP_REG:
                ins.ops.append(Op('reg', name=self._reg(op.reg), access=op.access))
            elif op.type == capstone.x86.X86_OP_IMM:
                ins.ops.append(Op('imm', imm=op.imm & 0xFFFFFFFFFFFFFFFF, access=op.access))
            elif op.type == capstone.x86.X86_OP_MEM:
                mem = op.mem
                ins.ops.append(Op('mem', base=self._reg(mem.base), index=self._reg(mem.index),
                                  scale=mem.scale, disp=mem.disp, access=op.access))
            else:
                ins.ops.append(Op('other', access=op.access))
        # Implicit writes (push/pop/call/ret/leave) and explicit ones, as parent registers.
        ins.writes = {parent64(self._reg(r)) for r in found.regs_write}
        for op in ins.ops:
            if op.kind == 'reg' and op.access & WRITE:
                ins.writes.add(parent64(op.name))
        # Relative branch target: capstone gives the absolute address of a branch immediate.
        ins.target = 0
        if ins.category in (CALL, COND, UNCOND) and ins.ops and ins.ops[0].kind == 'imm':
            ins.target = ins.ops[0].imm
        ins.changes_sp, ins.sp_delta = self._stack_pointer(ins)
        return ins

    def _stack_pointer(self, ins):
        """DecodeCodeInstruction: changes_stack_pointer (a write of rsp, explicit or hidden, that is
        not a call or a return) and stack_pointer_delta (a known constant change)."""
        explicit_write = any(op.kind == 'reg' and is_sp(op.name) and op.access & WRITE for op in ins.ops)
        hidden = ins.mnemonic in IMPLICIT_SP
        changes = (explicit_write or hidden) and ins.category not in (CALL, RET)
        if not changes:
            return False, None
        ops = ins.ops
        delta = None
        if ins.mnemonic in ('add', 'sub') and len(ops) >= 2 and ops[0].kind == 'reg' \
                and is_sp(ops[0].name) and ops[1].kind == 'imm':
            value = s32_fix(ops[1].imm)
            delta = value if ins.mnemonic == 'add' else -value
        elif ins.mnemonic == 'lea' and len(ops) >= 2 and ops[0].kind == 'reg' and is_sp(ops[0].name) \
                and ops[1].kind == 'mem' and is_sp(ops[1].base) and ops[1].index == '':
            delta = ops[1].disp
        elif ins.mnemonic in PUSH_MNEMONICS:
            delta = -8
        elif ins.mnemonic in POP_MNEMONICS:
            delta = 8
        return True, delta


class Function:
    __slots__ = ('start', 'end', 'ins', 'keys', 'branch_targets', 'indirect')

    def __init__(self, start, end, ins, branch_targets, indirect):
        self.start, self.end, self.ins = start, end, ins
        self.keys = sorted(ins)
        self.branch_targets = branch_targets
        self.indirect = indirect

    def previous_contiguous(self, address):
        """The decoded instruction that ends exactly where address starts, or None."""
        i = bisect.bisect_left(self.keys, address)
        if i == 0:
            return None
        prev = self.keys[i - 1]
        return prev if prev + self.ins[prev].length == address else None


def _writes(ins, reg):
    return reg in ins.writes


def resolve_jump_table(dec, func, branch_addr, seg_start, seg_end):
    """ResolveBoundedJumpTable: the targets of a `jmp reg` through a bounded table of 32-bit
    offsets, or None when it is not that pattern or the table is not unambiguous."""
    branch = func.ins[branch_addr]
    if branch.mnemonic != 'jmp' or not branch.ops or branch.ops[0].kind != 'reg':
        return None
    target_reg = parent64(branch.ops[0].name)

    add_addr = None
    cursor = branch_addr
    for _ in range(MAX_INTERVENING + 1):
        cand = func.previous_contiguous(cursor)
        if cand is None:
            break
        d = func.ins[cand]
        if d.mnemonic == 'add' and len(d.ops) >= 2 and d.ops[0].kind == 'reg' \
                and parent64(d.ops[0].name) == target_reg and d.ops[1].kind == 'reg':
            add_addr = cand
            break
        if _writes(d, target_reg) or d.is_terminator():
            return None
        cursor = cand
    if add_addr is None:
        return None
    table_reg = parent64(func.ins[add_addr].ops[1].name)

    load_addr = func.previous_contiguous(add_addr)
    if load_addr is None:
        return None
    load = func.ins[load_addr]
    if not (load.mnemonic == 'movsxd' and len(load.ops) >= 2 and load.ops[0].kind == 'reg'
            and parent64(load.ops[0].name) == target_reg and load.ops[1].kind == 'mem'
            and parent64(load.ops[1].base) == table_reg and load.ops[1].index != ''
            and load.ops[1].scale == 4):
        return None
    index_reg = parent64(load.ops[1].index)

    table_size = None
    guarded_start = None
    cursor = load_addr
    for _ in range(MAX_TABLE_SCAN):
        cand = func.previous_contiguous(cursor)
        if cand is None:
            break
        d = func.ins[cand]
        if d.mnemonic == 'cmp' and len(d.ops) >= 2 and d.ops[0].kind == 'reg' \
                and parent64(d.ops[0].name) == index_reg and d.ops[1].kind == 'imm':
            nxt = cand + d.length
            bounds = func.ins.get(nxt)
            if bounds is None:
                return None
            bound = s32_fix(d.ops[1].imm)
            if bound < 0:
                return None
            if bounds.mnemonic == 'ja':        # JNBE
                table_size = bound + 1
            elif bounds.mnemonic in ('jae', 'jnb'):  # JNB
                table_size = bound
            else:
                return None
            guarded_start = nxt + bounds.length
            break
        if _writes(d, index_reg):
            return None
        cursor = cand
    if table_size is None:
        return None
    if any(guarded_start <= t <= branch_addr for t in func.branch_targets):
        return None

    def table_address(d):
        if d.mnemonic != 'lea' or len(d.ops) < 2 or d.ops[0].kind != 'reg' \
                or parent64(d.ops[0].name) != table_reg or d.ops[1].kind != 'mem':
            return None
        mem = d.ops[1]
        if mem.base in RELOC_RIP_NAMES:
            return d.address + d.length + mem.disp
        if mem.base == '' and mem.index == '':
            return mem.disp & 0xFFFFFFFFFFFFFFFF
        return None

    candidates = set()
    cursor = load_addr
    for _ in range(MAX_TABLE_SCAN):
        cand = func.previous_contiguous(cursor)
        if cand is None:
            break
        d = func.ins[cand]
        if not _writes(d, table_reg):
            cursor = cand
            continue
        address = table_address(d)
        if address is not None:
            candidates.add(address)
        break
    if not candidates:
        for address in func.keys:
            if address >= branch_addr:
                break
            address_of_table = table_address(func.ins[address])
            if address_of_table is not None:
                candidates.add(address_of_table)
    if table_size == 0 or table_size > MAX_JUMP_TABLE:
        return None

    resolved = None
    for table in sorted(candidates):
        if table < seg_start or table > seg_end or table_size > (seg_end - table) // 4:
            continue
        targets = []
        valid = True
        for index in range(table_size):
            raw = dec.image.read(table + 4 * index, 4)
            if len(raw) != 4:
                valid = False
                break
            target = table + int.from_bytes(raw, 'little', signed=True)
            if target < func.start or target >= func.end:
                valid = False
                break
            targets.append(target)
        if not valid:
            continue
        if resolved is not None:
            return None
        resolved = sorted(set(targets))
    return resolved


def decode_function(dec, start, end, seg_start, seg_end):
    """DecodeFunction: the instructions reachable from the entry (following branches inside the
    function, then the resolvable jump tables), the internal branch targets and whether an
    indirect branch stays unresolved."""
    ins = {}
    branch_targets = set()
    visited = set()
    indirect = set()
    resolved = set()
    blocks = [start]
    while True:
        while blocks:
            address = blocks.pop()
            while start <= address < end and address not in visited:
                visited.add(address)
                d = dec.at(address, end)
                if d is None:
                    break
                target = d.target
                in_function = start <= target < end
                if in_function:
                    branch_targets.add(target)
                ins[address] = d
                if d.category == COND:
                    if in_function:
                        blocks.append(target)
                elif d.is_terminator():
                    if d.category == UNCOND and in_function:
                        blocks.append(target)
                    elif d.category == UNCOND and target == 0:
                        indirect.add(address)
                    break
                address = address + d.length
        discovered = False
        for branch in sorted(indirect):
            if branch in resolved:
                continue
            view = Function(start, end, ins, branch_targets, False)
            targets = resolve_jump_table(dec, view, branch, seg_start, seg_end)
            if targets is None:
                continue
            resolved.add(branch)
            for target in targets:
                branch_targets.add(target)
                if target not in visited:
                    blocks.append(target)
                    discovered = True
        if not discovered:
            break
    return Function(start, end, ins, branch_targets, len(indirect) != len(resolved))


def wrap_stack_refusal(func, start, time_mode=False):
    """WrapStackRefusal: the reason Wrap (or Time) must refuse this function, or None. It walks every
    path from the entry with the stack depth (0 = the return address at [rsp]) and rbp's copy of rsp."""
    state_at = {start: (0, NOT_STACK)}
    work = [start]
    while work:
        address = work.pop()
        depth, rbp = state_at[address]
        while True:
            d = func.ins.get(address)
            if d is None:
                break
            ops = d.ops
            mn = d.mnemonic
            for op in ops:
                if op.kind != 'mem':
                    continue
                if is_sp(op.index) or (is_bp(op.index) and rbp is not NOT_STACK):
                    return 'Wrap: the function indexes the stack'
                if is_sp(op.base):
                    if depth is None:
                        return 'Wrap: stack operand at an unknown stack depth'
                    if depth + op.disp >= 0:
                        return 'Wrap: the function reads its return address or stack arguments'
                if is_bp(op.base) and rbp is not NOT_STACK:
                    if rbp is None:
                        return 'Wrap: frame-pointer operand at an unknown stack depth'
                    if rbp + op.disp >= 0:
                        return 'Wrap: the function reads its return address or stack arguments'

            if time_mode:
                # sotc: Time only (see the comment at WrapStackRefusal in cpu_patches.cpp).
                writes_other = mn == 'push'
                for op in ops:
                    if not op.access & WRITE:
                        continue
                    stack_register = op.kind == 'reg' and (is_sp(op.name) or is_bp(op.name))
                    writes_other = writes_other or not stack_register
                for op in ops:
                    stack_value = False
                    offset = None
                    if op.kind == 'reg':
                        if is_sp(op.name):
                            stack_value, offset = True, depth
                        elif is_bp(op.name) and rbp is not NOT_STACK:
                            stack_value, offset = True, rbp
                    elif mn == 'lea' and op.kind == 'mem':
                        rsp_base = is_sp(op.base)
                        if rsp_base or (is_bp(op.base) and rbp is not NOT_STACK):
                            stack_value = True
                            base = depth if rsp_base else rbp
                            if base is not None and op.index == '':
                                offset = base + op.disp
                    if not stack_value or not writes_other:
                        continue
                    if mn not in ('mov', 'lea', 'push'):
                        return 'Time: a stack pointer is used in arithmetic'
                    if offset is None or offset >= -TIME_SLOT_MARGIN:
                        return 'Time: a register or memory value points near the return slot'
            if time_mode and mn == 'ret' and depth != 0:
                return 'Time: the return is not at the entry stack depth'
            if mn in POP_MNEMONICS and (depth is None or depth >= 0):
                return 'Wrap: a pop reads the return address'

            copies_rsp = (mn == 'mov' and len(ops) >= 2 and ops[0].kind == 'reg' and ops[1].kind == 'reg'
                          and is_sp(ops[1].name) and not is_sp(ops[0].name))
            if copies_rsp and not is_bp(ops[0].name) and (depth is None or depth >= 0):
                return 'Wrap: rsp is copied while it points at the return address'

            if mn == 'leave':
                if rbp is NOT_STACK or rbp is None or rbp >= 0:
                    return 'Wrap: leave without a known frame'
                depth, rbp = rbp + 8, NOT_STACK
            else:
                depth_before = depth
                if d.changes_sp:
                    depth = depth + d.sp_delta if (depth is not None and d.sp_delta is not None) else None
                writes_rbp = any(op.kind == 'reg' and is_bp(op.name) and op.access & WRITE for op in ops)
                if writes_rbp:
                    if copies_rsp:
                        rbp = depth_before
                    elif mn == 'lea' and len(ops) >= 2 and ops[1].kind == 'mem' and is_sp(ops[1].base) \
                            and ops[1].index == '':
                        rbp = None if depth_before is None else depth_before + ops[1].disp
                    else:
                        rbp = NOT_STACK

            state = (depth, rbp)
            next_address = address + d.length

            def follow(target):
                if target not in state_at:
                    state_at[target] = state
                    work.append(target)
                    return True
                return state_at[target] == state

            if d.category == COND:
                if d.target in func.ins and not follow(d.target):
                    return 'Wrap: the stack depth differs between paths'
            elif d.is_terminator():
                if d.category == UNCOND:
                    if d.target == 0:
                        return 'Wrap: the function has an indirect jump'
                    if d.target in func.ins and not follow(d.target):
                        return 'Wrap: the stack depth differs between paths'
                break
            if next_address in func.branch_targets:
                if not follow(next_address):
                    return 'Wrap: the stack depth differs between paths'
                break
            address = next_address
            depth, rbp = state
    return None


def relocatable(d):
    """Whether EncodeRelocatedInstruction can move the instruction to the trampoline. A relative
    branch with a rel8 form (2 bytes: jcc, jmp, loop*, jrcxz) cannot reach its target from the
    trampoline, which lies far from the eboot, so it is refused. This assumes the encoder keeps the
    short form of the original (ZydisEncoderDecodedInstructionToEncoderRequest); see the report."""
    relative = d.category in (COND, UNCOND, CALL) and d.ops and d.ops[0].kind == 'imm'
    if relative and d.category in (COND, UNCOND) and d.length == 2:
        return False
    return True


class Context:
    """The eboot as the detour engine sees it: function starts and bounds, earlier patches."""

    def __init__(self, image, sse4a=False, tsc_qpc=False):
        self.image = image
        self.dec = Decoder(image)
        self.module_start = 0
        self.module_end = max(base + len(data) for base, data, _ in image.segments)
        self.starts = sorted({start for start, _ in image.functions})
        self.patched_insns = set()
        self.patched_ranges = []
        self.red_zone_ranges = []
        self.function_cache = {}
        self.notes = []
        self._red_zone(sotc_cpu.SOTC_RED_ZONE)
        if sse4a or tsc_qpc:
            self._linear_patches(sse4a, tsc_qpc)

    def function_bounds(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i < 0:
            return None
        start = self.starts[i]
        end = self.starts[i + 1] if i + 1 < len(self.starts) else self.module_end
        return start, end

    def _red_zone(self, addresses):
        """The three functions sotc_cpu.py selects are protected at load (PatchRedZoneMemoryInstructions,
        selected mode). Modelled as whole functions: their red-zone rewrite and the spans it writes
        are not simulated, so a hook inside them is refused with the protection reason."""
        for address in addresses:
            bounds = self.function_bounds(address)
            if bounds is None:
                continue
            self.red_zone_ranges.append(bounds)
            self.patched_ranges.append(bounds)
            self.notes.append(f'red-zone function {bounds[0]:#x}-{bounds[1]:#x} (selected at {address:#x})')

    def _linear_patches(self, sse4a, tsc_qpc):
        """The SSE4a rewrites (only on CPUs without SSE4a) and the rdtsc rewrites (only when the
        cores disagree on the TSC). Approximated: each patched instruction is its own range, found by
        a linear sweep of every function."""
        names = set()
        if sse4a:
            names |= {'extrq', 'insertq', 'movntss', 'movntsd'}
        if tsc_qpc:
            names |= {'rdtsc'}
        for start, end in self.image.functions:
            address = start
            while address < end:
                d = self.dec.at(address, end)
                if d is None:
                    break
                if d.mnemonic in names:
                    self.patched_insns.add(address)
                    self.patched_ranges.append((address, address + max(d.length, NEAR_JUMP_SIZE)))
                address += d.length
        self.notes.append(f'CPU passes modelled: {", ".join(sorted(names)) or "none"} (approximate ranges)')

    def function(self, start, end, seg_start, seg_end):
        key = start
        if key not in self.function_cache:
            self.function_cache[key] = decode_function(self.dec, start, end, seg_start, seg_end)
        return self.function_cache[key]


def check_detour(ctx, mode, address, pattern_len):
    """InstallGuestDetour's checks for one site. mode: entry, wrap, site or time. Returns
    (accepted, reason, displaced_bytes)."""
    if not (ctx.module_start <= address < ctx.module_end):
        return False, 'address is outside the registered module', 0
    if pattern_len > ctx.module_end - address:
        return False, 'pattern runs past the module', 0
    if not ctx.starts:
        return False, 'no function table (the eh_frame passes record it)', 0
    idx = bisect.bisect_right(ctx.starts, address)
    if idx == 0:
        return False, 'address is before the first function', 0
    function_start = ctx.starts[idx - 1]
    if mode != 'site' and function_start != address:
        return False, 'not a function entry', 0
    function_end = ctx.starts[idx] if idx < len(ctx.starts) else ctx.module_end

    for begin, end in ctx.red_zone_ranges:
        if begin <= address < end:
            return False, 'inside a red-zone-protected function', 0

    function = ctx.function(function_start, function_end, ctx.module_start, ctx.module_end)
    if function.indirect:
        return False, 'function has an unresolved indirect branch', 0
    if mode in ('wrap', 'time'):
        for address_of_ins in sorted(function.ins):
            d = function.ins[address_of_ins]
            if d.mnemonic.startswith('ret') and d.ops:
                return False, ('Wrap: the function pops stack arguments' if mode == 'wrap'
                               else 'Time: the function pops stack arguments'), 0

    for begin, end in ctx.patched_ranges:
        if begin < function_end and function_start < end:
            return False, 'the function has other CPU patches (its branch targets are unknown)', 0

    if mode != 'site' and address in function.branch_targets:
        return False, 'a branch inside the function targets its entry', 0

    span = []
    continuation = address
    while continuation - address < NEAR_JUMP_SIZE:
        d = function.ins.get(continuation)
        if d is None:
            return False, 'displaced span is not decoded', 0
        if continuation != address and continuation in function.branch_targets:
            return False, 'branch target inside the displaced span', 0
        if continuation in ctx.patched_insns:
            return False, 'displaced span overlaps another CPU patch', 0
        span.append(d)
        continuation += d.length
    for i in range(len(span) - 1):
        if not span[i].is_detour_control_flow():
            continue
        if i == 0 and mode != 'site':
            continue
        return False, 'control flow inside the displaced span', 0
    if continuation > function_end:
        return False, 'displaced span crosses the function end', 0

    if mode == 'time':
        for d in function.ins.values():
            if d.category not in (UNCOND, COND):
                continue
            if d.target < function_start or d.target >= function_end:
                return False, 'Time: a branch leaves the function (tail call)', 0
    if mode in ('wrap', 'time'):
        reason = wrap_stack_refusal(function, address, mode == 'time')
        if reason:
            return False, reason, 0

    for begin, end in ctx.patched_ranges:
        if address < end and begin < continuation:
            return False, 'displaced span overlaps another CPU patch', 0

    for d in span:
        if not relocatable(d):
            return False, 'displaced instruction cannot be relocated', 0
    return True, '', continuation - address


def span_of(ctx, address):
    """The displaced byte range of an accepted detour, for the conflict report."""
    start, end = ctx.function_bounds(address)
    function = ctx.function(start, end, ctx.module_start, ctx.module_end)
    continuation = address
    while continuation - address < NEAR_JUMP_SIZE and continuation in function.ins:
        continuation += function.ins[continuation].length
    return address, continuation


def check_records(ctx, records, resolutions):
    """Per record: the reason it is refused (None when accepted), in the loader's order of effects:
    resolution first, then the detour, then dependencies, anchors and groups (Propagate)."""
    own = []
    detail = []
    for record, result in zip(records, resolutions):
        if result.state == 'off':
            own.append(f'off({result.reason})')
            detail.append(None)
            continue
        mode = record['mode']
        if mode == 'range':
            own.append(None)
            detail.append(('range', 0))
            continue
        detour_mode = 'time' if mode == 'time' else ('entry' if mode == 'entry' else
                                                     ('wrap' if mode == 'wrap' else 'site'))
        accepted, reason, displaced = check_detour(ctx, detour_mode, result.site, len(record['pattern']))
        own.append(None if accepted else reason)
        detail.append((detour_mode, displaced) if accepted else (detour_mode, 0))

    reasons = list(own)
    changed = True
    while changed:
        changed = False
        for i, record in enumerate(records):
            if reasons[i] is not None:
                continue
            for dep in record['deps']:
                if reasons[dep] is not None:
                    reasons[i] = f'dep {records[dep]["name"]}'
                    changed = True
                    break
            if reasons[i] is None and record['anchor'] is not None and reasons[record['anchor']] is not None:
                reasons[i] = f'anchor {records[record["anchor"]]["name"]}'
                changed = True
        for failed, record in enumerate(records):
            group = record['group']
            if not group or reasons[failed] is None:
                continue
            for j, member in enumerate(records):
                if member['group'] == group and reasons[j] is None:
                    reasons[j] = f'group {group}: {records[failed]["name"]}'
                    changed = True
    return reasons, detail


def build_report(eboot_path, hooks_dir, include_examples, ctx_flags):
    data = eboot_path.read_bytes()
    image, fingerprint = hooks_check.load_image(data)
    declarations = sotc_hooks.load_declarations(hooks_dir, include_examples)
    records, known = sotc_hooks.build(declarations, fingerprint)
    decoded = sotc_hooks.decode(sotc_hooks.encode(fingerprint, known, records))['records']
    resolutions = hooks_check.resolve(decoded, image)
    ctx = Context(image, **ctx_flags)
    reasons, detail = check_records(ctx, decoded, resolutions)
    return ctx, decoded, resolutions, reasons, detail, fingerprint, known


def run_validation(ctx):
    """The known outcomes: three refusals the loader logged, and eleven hooks that installed in real
    runs. The refusals with a mode the declarations do not use (0x19b760 as wrap) are checked directly."""
    cases = [
        ('refusal', 'PC_POOL_ALLOC', 0x19e00, 'wrap', 'function has an unresolved indirect branch'),
        ('refusal', 'XPRMEM_BUILD_MASKS@wrap', 0x19b760, 'wrap', 'Wrap: stack operand at an unknown stack depth'),
        ('refusal', 'entry@0x136f0', 0x136f0, 'entry', 'function has an unresolved indirect branch'),
    ]
    rows = []
    ok = True
    for kind, name, site, mode, reason in cases:
        accepted, got, _ = check_detour(ctx, mode, site, 16)
        match = (not accepted) and got == reason
        ok &= match
        rows.append((name, site, mode, 'refused' if not accepted else 'accepted', got, reason, match))
    installed = {
        'ALLOC_BLOCK': (0x479a0, 'wrap'), 'FREE_BLOCK': (0x1b0e0, 'entry'),
        'MEMTYPE_ALLOC': (0x12820, 'wrap'), 'MEMTYPE_FLIP_SITE': (0x8c860, 'entry'),
        'XPRMEM_BUILD_MASKS': (0x19b760, 'entry'), 'XPRMEM_PRERENDER_BLOCKS': (0xca150, 'wrap'),
        'PC_RING_ALLOC': (0x12820, 'wrap'), 'PC_RING_BUMP': (0x48440, 'entry'),
        'XPR_CONSUME_MASK': (0x1a6d20, 'entry'), 'PARTICLE_COUNT_READ': (0x8c1b0, 'entry'),
        'ENGINE_CMD_TICK': (0x3c7740, 'entry'),
    }
    for name, (site, mode) in installed.items():
        accepted, got, displaced = check_detour(ctx, mode, site, 16)
        ok &= accepted
        rows.append((name, site, mode, 'accepted' if accepted else 'refused', got,
                     'accepted', accepted))
    return ok, rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--eboot', type=Path, default=ROOT / 'out' / 'eboot.elf',
                        help='an ELF, an eboot.bin or a game folder (default out/eboot.elf)')
    parser.add_argument('--hooks-dir', type=Path, default=sotc_hooks.HOOKS_DIR, help=argparse.SUPPRESS)
    parser.add_argument('--include-examples', action='store_true',
                        help='also check the files whose names start with "_"')
    parser.add_argument('--allow-refused', action='store_true', help='report refusals, exit 0')
    parser.add_argument('--validate', action='store_true',
                        help='check the built-in validation table (known refusals and installs)')
    parser.add_argument('--sse4a', action='store_true',
                        help='model the SSE4a rewrites (a CPU without SSE4a)')
    parser.add_argument('--tsc-qpc', action='store_true',
                        help='model the rdtsc rewrites (the cores disagree on the TSC)')
    args = parser.parse_args(argv)
    eboot = args.eboot / 'eboot.bin' if args.eboot.is_dir() else args.eboot
    if not eboot.is_file():
        print(f'hooks_install_check: {eboot} not found (run sotc.py once to prepare out/eboot.elf, or pass --eboot)',
              file=sys.stderr)
        return 2
    try:
        ctx, records, resolutions, reasons, detail, fingerprint, known = build_report(
            eboot, args.hooks_dir, args.include_examples,
            {'sse4a': args.sse4a, 'tsc_qpc': args.tsc_qpc})
    except (sotc_hooks.DeclarationError, ValueError) as error:
        print(f'hooks_install_check: {error}', file=sys.stderr)
        return 2

    for note in ctx.notes:
        print(f'note: {note}')
    refused = 0
    for record, result, reason, (mode, displaced) in zip(records, resolutions, reasons, detail):
        site = result.site if result.state != 'off' else 0
        if reason is None:
            extra = f' displaced={displaced}' if mode != 'range' and displaced else ''
            print(f'SOTC DETOUR {record["name"]} {site:#x} accepted mode={record["mode"]}{extra}')
        else:
            refused += 1
            print(f'SOTC DETOUR {record["name"]} {site:#x} refused mode={record["mode"]}: {reason}')

    conflicts = []
    accepted_spans = []
    for record, result, reason in zip(records, resolutions, reasons):
        if reason is None and record['mode'] != 'range' and result.state != 'off':
            accepted_spans.append((record['name'], *span_of(ctx, result.site)))
    for i, (name_a, a0, a1) in enumerate(accepted_spans):
        for name_b, b0, b1 in accepted_spans[i + 1:]:
            if a0 < b1 and b0 < a1:
                conflicts.append(f'{name_a} and {name_b} displace overlapping bytes')
    for line in conflicts:
        print(f'SOTC DETOUR CONFLICT {line} (both enabled: the later one is refused)')

    total = len(records)
    print(f'hooks_install_check: {total - refused}/{total} hooks accepted by the detour engine; '
          f'eboot {fingerprint[:12]} ({"declared offsets checked" if known else "no declared offsets for this eboot"})')

    status = 0
    if args.validate:
        ok, rows = run_validation(ctx)
        print('validation:')
        for name, site, mode, got, reason_got, expected, match in rows:
            verdict = 'ok' if match else 'MISMATCH'
            print(f'  {verdict:8} {name:28} {site:#x} {mode:6} {got}: {reason_got} (expected {expected})')
        print(f'validation: {"all as expected" if ok else "MISMATCHES"}')
        if not ok:
            status = 1
    if refused and not args.allow_refused:
        status = status or 1
    return status


if __name__ == '__main__':
    sys.exit(main())
