# Indirect callers: how the functions with no direct caller are reached (RE, topic T6)

Scope: the functions that earlier agents found with no direct caller: the T4 item builders and V# setters, the D2
step-rate writer and generic setters, the K2 GS/HS setters, and the R8 draw packet builder `0x862c50`. Plus the
writers of the graphics-context fields `O+0xc0e8` and `O+0xc108`.
Evidence: the eboot image `out/eboot.elf` (PS4 ET_SCE_EXEC, file bytes already hold the final pointer values; see 1.2),
the function index of `tools/re/eboot_re.py` (`out/re/index.pkl.gz`: direct calls, tail jumps, RIP-relative refs),
Ghidra decompile in `out/re/decomp/<hi16>/<addr>.c`, and the new script `tools/re/indirect_refs.py` (stdlib + capstone
5.0.7; its capstone pass is cached in `out/re/indirect_scan.pkl.gz`).
Addresses are eboot VAs. Labels: **observed** (read in bytes or decomp), **inferred** (reasoning), **unknown**.
Read-only: no source, build, game run or git change.

---

## 0. Summary

Status per function (REACHED = a static path from code was found; DEAD = no static path of any kind was found;
UNKNOWN = the chain ends in a function with no static reference).

| Function | Status | How it is reached (or not) | Evidence |
|---|---|---|---|
| `0x103ad0` (T4 item, object mesh) | **REACHED** via a function table | tail jump from `0x103aa0` (`0x103ab4`); `0x103aa0` is entry 0 of a 7-entry function table at `0x15fea00` | section 2.1 |
| `0x1051f0` (T4 item) | **REACHED** via a function table | tail jump from `0x103aa0` (`0x103abc`), same chain | section 2.1 |
| `0xdb700` (T4 item, indirect draw) | **REACHED** direct | 8 call or tail-jump sites (section 2.2) | index |
| `0xcaed0` (T4 item, pass runner) | **REACHED** direct | `0xccf74` (in `sub_ccdf0`), `0xcf88f` (in `EndFrameViewParams`) | index |
| `0x1b0980` (T4 item) | **REACHED** direct | `0x19aeda` in `sub_19aad0`; `sub_19aad0` is called at `0x2723b2` (`sub_271b10`) | index |
| `0xaea10` (T4 item) | **REACHED** direct | `0xc62e3` in `BPRender_DepthAndGBufferPasses` (`0xc59d0`) | index |
| `0x8615f0` (T4 setter `O+0xc130`/`0xc168`) | **DEAD** (static) | no pointer, no branch, no table entry | 2.4 |
| `0x861730`, `0x861880` (CS setters, `O+0xc0d8`) | **DEAD** (static) | same; the live CS setter is reached by its own callers (not traced here) | 2.4 |
| `0x860c50` (K2 GS setter) | **UNKNOWN** chain | tail jump at `0x94aa5` from `sub_94a80`, which has no static reference | section 3 |
| `0x8612b0` (K2 HS setter) | **UNKNOWN** chain | tail jump at `0x94a18` from `sub_949f0`, which has no static reference | section 3 |
| `0x860ea0`, `0x8613e0` (K2 GS/HS setters) | **DEAD** (static) | no reference of any kind | 2.4 |
| `0x86bb90` (D2 step-rate writer) | **DEAD** (static) | no reference of any kind | 2.4 |
| `0x86bb30`, `0x869350`, `0x8694e0`, `0x869370` (D2) | **DEAD** (static) | no reference of any kind | 2.4 |
| `0x862c50` (R8 draw packet builder) | **DEAD** (static) | no reference of any kind; the "vtable slot" idea is refuted | 2.4 |
| writers of `O+0xc0e8`, `O+0xc108` | **UNKNOWN** | no store to these offsets exists in the code (section 4) | section 4 |

The "DEAD" label means: no pointer to the function exists in the eboot bytes, no direct or tail branch reaches it,
and no RIP-relative or table entry reaches it. It does not prove that no code can ever run it: a register-based
dispatch table filled at run time, or an address built by arithmetic, would not show up here (section 5).

---

## 1. Method

### 1.1 What the scan checks (`tools/re/indirect_refs.py`)

For each target:
1. **Direct calls and tail jumps** from the index (`call rel32`, and `jmp rel32` to a function start).
2. **RIP-relative references** from the index (`lea`, `mov`, `vmovq` with `[rip+...]`) to the target.
3. **Absolute 8-byte pointers** in every segment with file bytes, including the R+X segment that holds `.rodata`
   (the first PT_LOAD, `0x0..0x1593978`). A pointer slot inside a run of neighbouring function starts is a vtable
   or function table; its base and index are reported, and the code that references the base (constructor or
   reader) is listed.
4. **Field-relative 4-byte offsets** (target = field address + s32). This is the engine's relative-pointer convention
   mentioned in R1 (`.cslt` uses s32 offsets relative to the field).
5. **Table-base 4-byte offsets** (target = base + s32[k], k < 512). The bases are every RIP-relative target in the image
   plus the absolute displacements of memory operands (107 of them).
6. **Raw matches in code** of the target as 4-byte or 8-byte immediates (not decoded; a hit would need a look).
7. **Indirect calls and jumps** with `[reg+disp]` (30,702 sites), with the displacement equal to 8 * vtable index, and
   **indexed dispatch** sites `call/jmp [reg + reg*8 + disp]` (70 sites).
8. **Stores** with displacement `0xc0c0..0xc118` into any register (covers `O+0xc0e8` and `O+0xc108`).

### 1.2 Relocations

`out/boot.bin` has 709,058 kind-0 relocations. For every one of them the file bytes at the target already hold the
relocation value (checked: 709,058 of 709,058, addend 0 in all). So the absolute-pointer scan on the file is complete
for data pointers. No relocation value equals any target of this document.

### 1.3 Counts

- 55,790 functions (FDE starts). All 20 original targets and the 12 chain functions are FDE starts.
- Absolute pointers to a function start in data: 59,164 slots. To any target of this document: 1 (`0x103aa0`).
- Table bases: 51,455. Table-base hits: none that are real (section 5.2).

---

## 2. T4 item builders and the setters of the mesh and the CS path

### 2.1 `0x103ad0` and `0x1051f0`: reached through `0x103aa0` and a 7-entry function table

- `sub_103aa0` (`0x103aa0`) is the only function that tail-jumps to both builders: `0x103ab4 jmp 0x103ad0` (taken when
  `rdx != 0`) and `0x103abc jmp 0x1051f0` (**observed**).
- `0x103aa0` is entry 0 of a 7-entry table at `0x15fea00` (**observed**, file bytes):

  | index | entry | name in index |
  |---|---|---|
  | 0 | `0x103aa0` | `sub_103aa0` |
  | 1 | `0x2a5400` | `sub_2a5400` |
  | 2 | `0x106920` | `sub_106920` |
  | 3 | `0x3a5ac0` | `sub_3a5ac0` |
  | 4 | `0xb5060` | `sub_b5060` |
  | 5 | `0x3f9420` | `sub_3f9420` |
  | 6 | `0x3e35d0` | `sub_3e35d0` |

- The only code reference to `0x15fea00` is `0xd039e` in `sub_d0040` (`0xd0040`, 1,177 bytes, called from
  `EndFrameViewParams`, `0xcefa0`): `vmovq xmm1, qword ptr [rip+0x152e65a]`, an 8-byte **load** of entry 0 (**observed**).
  It is not a constructor store of a vptr. The table is probably copied from this function into an object, and an
  indirect call through the object reaches entry 0 (**inferred**).
- The indirect call that uses the copy is **unknown**. Every `call [reg+0]` in the eboot matches (2,226 sites), so the
  scan cannot pick it out. The table is the chain's root, not a slot with a known caller.
- Consequence for T4: the item builders `0x103ad0` and `0x1051f0` are reached, not dead. Their chain is the object
  mesh path (`0x10c2e0`, T4.8) through `0x103aa0`, not a direct call.

### 2.2 `0xdb700`, `0xcaed0`, `0x1b0980`, `0xaea10`: direct callers

| Target | Direct sites (index) |
|---|---|
| `0xdb700` | `0xc1ab7`, `0xc1aee` (`sub_c1860`); `0xc1cb4` (`sub_c1c80`); `0xc2270`, `0xc22a6` (`sub_c1fc0`); `0xc4900`, `0xc4937` (`sub_c46c0`); `0xcea29` (`sub_ce850`) |
| `0xcaed0` | `0xccf74` (`sub_ccdf0+0x184`); `0xcf88f` (`EndFrameViewParams+0x8ef`) |
| `0x1b0980` | `0x19aeda` (`sub_19aad0+0x40a`); `sub_19aad0` is called at `0x2723b2` (`sub_271b10+0x8a2`) |
| `0xaea10` | `0xc62e3` (`BPRender_DepthAndGBufferPasses+0x913`, function `0xc59d0`); `0xc59d0` is called at `0xcb396` (`RenderView::MainCamera+0x4c6`) |

These four are reached by ordinary calls. The brief's question "who calls them" is answered by this table.

### 2.3 `0x1b0980` and `0xaea10`: the caller chains above

`0x1b0980` is 12 bytes (`0x1b0980..0x1b098c`), so it is probably a thunk to a larger body. Not checked further.

### 2.4 Dead setters and the R8 draw packet builder

For `0x8615f0`, `0x861730`, `0x861880`, `0x860ea0`, `0x8613e0`, `0x862c50`, `0x86bb30`, `0x86bb90`, `0x869350`,
`0x8694e0`, `0x869370`: no direct call, no tail jump, no RIP-relative reference, no absolute pointer, no table entry,
no raw immediate (**observed**, the scan output section "== 0x..."). Their only static relation is the stores they
themselves make:

- `0x8615f0`: `mov [rdi+0xc168], esi` (`0x8615f0`), `mov [rdi+0xc130], rdx` (`0x8615f6`).
  The live writer of the same two fields is `0x861520` (3 direct callers): `0x8615bf` and `0x8615c6` (**observed**).
  So `0x8615f0` is a duplicate of the VS path that is not called.
- `0x861730`: `mov [rdi+0xc130], rcx` (`0x861851`), `mov [rdi+0xc168], edx` (`0x861858`), and `mov [rdi+0xc0d8], r14`
  (`0x861785`). `0x861880`: `mov [rdi+0xc0d8], rsi` (`0x8618a5`) only. So `0x861880` is the CS-slot setter and `0x861730` writes both
  the CS slot and the `0xc130`/`0xc168` pair (**observed**).
- `0x860ea0`: `mov [rbx+0xc130], 0` (`0x861020`), `mov [rbx+0xc168], 0` (`0x86102b`). `0x8613e0`: `mov [rbx+0xc0c8], r14`
  (`0x8614bd`). These are dead copies of the K2 setters.
- `0x862c50` (R8 "vtable slot, not found"): there is no vtable that holds it. The hypothesis of a vtable is **refuted**.

---

## 3. K2 GS and HS setters: reached through two unresolved stage handlers

- `0x860c50` (GS setter, writes `O+0xc130`, `O+0xc168`) has one tail jump: `0x94aa5 jmp 0x860c50` inside `sub_94a80`
  (`0x94a80`). `0x8612b0` (HS setter, writes `O+0xc0c8`) has one tail jump: `0x94a18 jmp 0x8612b0` inside `sub_949f0`
  (`0x949f0`) (**observed**).
- `sub_94a80` and `sub_949f0` have **no** static reference: no pointer, no RIP-relative reference, no direct call, no
  table entry (**observed**). Their decompile headers say "called by:" with nothing after it.
- Their body is the same shape as `sub_94a20` and `sub_94ab0`: they store the stage record at
  `param_1[*(uint*)(param_1[4]+N) + 0x3b]` (the slot array of the pass, see slot-stages.md 3.2), compare the record
  with the previous one, and write a flag. The flag store is decompiled as `FUN_0000c540[idx] = ...` (**observed**),
  which Ghidra does not resolve. So the handlers look like the per-stage entries of a dispatch, and their callers are
  a computed dispatch (**inferred**).
- `sub_94ab0` is a direct caller target (`0xa69d0`, `0xadd50`, `0xaf610` call it), so only the GS/HS pair is in this
  unresolved group.
- **Status: UNKNOWN.** The `0x860c50` and `0x8612b0` setters are reached only through the two handlers, and the
  handlers are reached only through a dispatch the scan does not resolve.

---

## 4. Writers of `O+0xc0e8` and `O+0xc108`

- The only code that touches `O+0xc0e8` and `O+0xc108` is `0x85f410` (lines 111 and 114) and `0x85fc10` (lines 443 and 446).
  Both pass `param_1 + 0xc0e8` and `param_1 + 0xc108` as the **address** of an 8-byte SGPR source to `FUN_0086d5e0`
  (the PM4 `SET_SH_REG` writer, T4.5). They read the values; they do not write them (**observed**).
- No store with displacement `0xc0e8` or `0xc108` exists in the code (scan of stores at `0xc0c0..0xc118`, section 1.1).
  The only stores in that range are: `ymmword [reg+0xc0c0]` (32 bytes, covers `0xc0c0..0xc0df`) in `0x85ed20`,
  `0x85f080`, `0x85f100`; `qword [reg+0xc0c0]` in `0x860bb0`, `0x860c00`; `qword [reg+0xc0d0]` in `0x861260`;
  `qword [reg+0xc0c8]` in `0x8612b0`, `0x8613e0`; `qword [reg+0xc0d8]` in `0x861730`, `0x861880`; `dword [reg+0xc0fc]` in `0x8618e0`
  (**observed**).
- **Status: UNKNOWN.** The values at `0xc0e8` and `0xc108` are probably written as part of a block copy whose start
  is below `0xc0c0` (a `rep movs`, a `memcpy` import, or a store through a register that is not `reg+disp`). The three
  `ymmword` stores at `0xc0c0` are the next candidates to check at run time (T4.10 step 2).

---

## 5. Limits and what would change the status

1. **Register-based dispatch tables.** 70 indexed call and jump sites exist (`call qword ptr [r12 + rax*8 - 0x1d8]` in
   `sub_9e4f90`, `jmp qword ptr [rcx + rax*8]` in `sub_b16420`, and others). Their table base is in a register, so the
   scan cannot list the tables they use. A DEAD result is therefore "no static pointer", and it would be wrong if one of
   these tables were filled at run time with a pointer computed by arithmetic.
2. **Table-base hits are noise.** The table-base scan gave hits for `0x86bb30`, `0x86bb90` (4 each) and `0x19aad0`
   (3) and `0x860c50` (1). Inspected: the hits for `0x86bb30` and `0x86bb90` are four copies of the same
   template dword (`-0xa24340` / `-0xa24370`), spaced 0x30 apart in `.rodata` at `0x128fe70..0x128ff60`, which are PM4
   packet templates (engine-gnm-layer.md 3.2), not a jump table (**inferred**). The others were not inspected one by one.
   No hit has a run of two or more valid entries, which a jump table would have.
3. **Field-relative hits are single and inside code or rodata.** Each target has 1 or 2 field-relative hits; they are
   isolated words in the R+X segment, not a run of table entries (**inferred** as coincidences).
4. **Vtable calls.** `0x103aa0` is in a table with 2,226 `call [reg+0]` sites. The scan does not tie a call to an object
   of a class, so the actual call site of entry 0 is **unknown**.

---

## 6. Corrections to earlier documents

- **`re-pipelines/vertex-formats.md`, T4.10 item 7:** "the callers of the item builders: `0x103ad0` (no static
  callers)" is wrong. `0x103ad0` and `0x1051f0` are tail-jumped from `0x103aa0`, which is entry 0 of the table at
  `0x15fea00` (section 2.1). The other item builders have direct callers (section 2.2). The writers of `O+0xc0e8` and
  `O+0xc108` are still unknown (section 4). The callers of `0x8615f0`, `0x861730`, `0x861880` are none (section 2.4).
- **`re-pipelines/instancing.md`, section 0 item 1:** "It has no static callers" is confirmed for every kind of static
  reference (section 2.4). The step-rate writer `0x86bb90` is not reached by any pointer, branch or table in the eboot
  image. The conclusion that the eboot never writes the step rates stays **inferred**, with a stronger basis; the
  runtime check in D2 section 6 is still needed for dispatch tables not seen here.
- **`re-pipelines/slot-stages.md`, section 4:** the brief's "no direct callers" for the K2 setters holds for `0x860ea0`
  and `0x8613e0` (DEAD, section 2.4). It is wrong for `0x860c50` and `0x8612b0`: each has one tail jump from a stage
  handler (`sub_94a80`, `sub_949f0`, section 3). Those handlers are unresolved, so the GS and HS setters are UNKNOWN,
  not dead. The table in slot-stages.md section 4 itself is not changed by this.
- **R8 / K1 (`0x862c50`):** the "vtable slot, not found" note is refuted for the eboot as loaded. No vtable and no other
  pointer holds `0x862c50`.

---

## 7. Evidence files

- Script: `tools/re/indirect_refs.py` (run: `python tools/re/indirect_refs.py --rescan --extra 103aa0 94a80 949f0 ...`).
- Scan output used here: `scratchpad/indirect_out6.txt` (session scratchpad, not in the repo).
- Decomp: `out/re/decomp/000d/000d0040.c` (the `0xd039e` load),
  `out/re/decomp/0009/00094a80.c`, `0009/000949f0.c`, `0009/00094a20.c`, `0009/00094ab0.c`,
  `0085/0085f410.c` (lines 111, 114), `0085/0085fc10.c` (lines 443, 446), `0085/0085f220.c` (lines 63, 66),
  `0086/00861730.c`, `0086/00861880.c`.
- Index: `out/re/index.pkl.gz` (built by `tools/re/eboot_re.py build`).

## Run check (4 Nov 2026, SOTC_CALL_PROBE=1, log user/measure1/measure1.log)

One run of the Lab test route (boot, title, first load, then the route through home_north, home, home_south,
arena_01_entrance), with all 16 entry hooks installed ("expected" in the manifest), plus the port-side counter of
SET_CONTEXT_REG writes to 0xA2A8/0xA2A9:
- 0x103aa0 ran 364 times, always called from eb:0x39a73, inside JobSystemScheduler_ExecuteJob
  (0x39800-0x39cb8). So the 7-entry table at 0x15fea00 holds job functions, and the particle update
  (0x103ad0/0x1051f0) runs as a job of the engine's job system.
- 0 calls for 0x86bb90, 0x86bb30, 0x862c50, 0x8615f0, 0x861730, 0x861880, 0x860ea0, 0x8613e0, 0x869350,
  0x8694e0, 0x869370, the GS/HS setters 0x860c50 / 0x8612b0, and their stage handlers 0x94a80 / 0x949f0.
- 0 PM4 writes to VGT_INSTANCE_STEP_RATE_0/1 from any path (the counter printed no line).
Within this scenario, the step rates are never written, the 0x54-byte draw-packet builder 0x862c50 never runs, and
no GS or tessellation stage is bound. Other scenarios (boss fights, cutscenes, other areas) are not covered by this
run.
