# Writes wave 7, group "particles": RIPs 0x101ae4, 0x119e27, 0x11bd99

Specialist report (static analysis only: decompilation in `out/re/decomp`, `tools/re/eboot_re.py`, the verify6
numbers in `verify6-consumed.md`). No source edited, nothing built, nothing run, nothing committed.
Labels: **O** observed in decomp or disassembly, **I** inferred, **U** unknown.

## Summary

| rip | function | object written | class | hook proposal |
|---|---|---|---|---|
| 0x101ae4 | ParticleSystemPool::ProcessStats 0x1018c0 | per-pool "last alive" stamp table, `pool+0x188[u]` (u32, one per system) | (c) CPU-only bookkeeping, pending one runtime check (section 1) | no (fallback spec in section 1.6) |
| 0x119e27 | FUN_00118e10 (0x118e10, spot-light / shadow setup) | per-light constant block, store at `+0x80` | not particle: lights/shadow group (d for us) | no (route to lights group) |
| 0x11bd99 | FUN_0011b6d0 (0x11b6d0, draw-list emitter, pass 0x2a) | 32-byte sorted draw-list records | not particle: generic draw-list / instance writer | no (route to draw-list group) |

## 1. RIP 0x101ae4 (ParticleSystemPool::ProcessStats)

### 1.1 What the instruction writes (O)

- Disassembly 0x101ad1..0x101ae7: `jle 0x101c92` (c < 1 exits the active branch), then
  `mov rax,[r13+0x188]` (table base), `mov ecx,[rbp-0x54]` (value), `mov [rax+rbx*4],ecx` at 0x101ae4, with
  `rbx` = system index `u`, `r13` = pool.
- Value: `iVar3` = `*(int*)(param_3 + 0x104f8 + cat*0x18)`, the frame clock in ms of the system's category
  (`FUN_00109970` computes it at 0x104f8). So the write stores "now" for system u.
- Path: the `else` branch of `if (iVar4 < 1)` in the decomp, i.e. the write happens for every active system with a
  count `c >= 1` in this call. Once per pool per frame (ProcessStats is called once per pool from
  `FUN_00109970`, loop at 0x1099f0..0x109a09).
- Table size: `N = pool+0x1a8` (u64) = `pool+0x6c`, the system capacity of the pool (ctor `FUN_000fffa0`, decomp
  line `*(ulong *)(param_1 + 0x1a8) = uVar15`). **O**

### 1.2 Who allocates it, and in which memory category (O)

- Ctor `FUN_000fffa0` (called from `FUN_00102590`, pool create): `FUN_00017090(N<<2)` gives the stamp table at
  `pool+0x188`. `FUN_00017090` is `FUN_00012820(size, 1, 8, 2)` (decomp 0x17090). **O**
- Memory category 1 = **CPUMemory** (framedata.md 1.1; `0x12820` with `DAT_02dfaab0[1]`). Bucket allocator with page
  0x10000 for sizes up to 0x400, heap allocator above (allocators.md / framedata.md). **O** (from the existing notes)
- The same category holds the per-frame instance and skinning blocks of `0x8c0d0`, which framedata.md says the GPU
  reads. **O** (existing notes; this report does not re-verify it)
- Same ctor, the pool's GPU count rings: `FUN_0008be60(pool+0xf8, 0x1b, 4, N, 1, 0, 0)` (and +0xa8, +0xd0, +0x120,
  +0x148). Flag bit 0 set means category `DAT_02dd9f80` = CPUMemory (framedata 1.1). So the GPU-written stats ring
  (`pool+0xf8`, read by the CPU through `0x8c1b0`) and the stamp table are both N*4-byte allocations in CPUMemory,
  allocated in the same ctor. **O**

### 1.3 Does the GPU read it? (O for what was searched, I for the conclusion)

- Writers of `pool+0x188`: `0x100b80` (CreateInstance: stamp = spawn time, once per create), `0x1018c0` (this RIP).
  Free: `0x100730`. Alloc: `0xfffa0`. **O**
- Readers: none. Every `0x188` reference in the particle functions and their callees was checked (`0x1018c0`,
  `0x103ad0`, `0x1051f0`, `0x107540`, `0x109ca0`, `0x101720`, `0x1010a0`, `0x10bfd0`, `0x10c140`, `0x10c2e0`,
  `0x10c4e0`, `0x10c6c0`, `0x1097d0`, `0x10a500`..`0x10abe0`, `0x1028b0`, `0x100ad0`, `0xfc2d0`, `0xfc470`,
  `0xfc0f0`): no read. **O**
- No V# or T# is built from `pool+0x188`. The only pool array accessor on the GPU side is `0x8c1b0`, used on the
  count ring `pool+0xf8 + slot*0x28` (in `0x1018c0` and `0xe48b0`). **O**
- Conclusion: no particle shader or descriptor is known to read the stamp table as a buffer. The write is CPU
  bookkeeping (the lifetime timestamp of consumer `a` in impl-particles.md: AutoDestroy age). **I**

### 1.4 Why verify6 still counts it as consumed (I)

- Consumed means the old path re-uploaded the written bytes within 4 frames, found through a bind of at most
  64 KiB. The largest bind is exactly 65536, the size of a CPUMemory bucket page (0x10000). The table is in the same
  category and allocation neighbourhood as the GPU count ring (1.2), and the GPU-read per-frame blocks share the
  category too.
- So the most likely reading: a GPU bind (a V# over a neighbouring CPUMemory allocation, or over a page-sized
  window) covers the 4 bytes of the stamp table without reading them. That is the "shares a tracked page" case from
  the brief. **I** This is not proven: a V# whose base is exactly `pool+0x188` would give the same numbers and
  would be a real GPU read. The static search did not find such a V#, but a V# built inside generic code is not
  excluded by it.

### 1.5 The check that decides it (proposal for the next verify, no code written here)

For each consumed 0x101ae4 write, log the bind base and size that covered it, and the table base
`pool+0x188` and `N = pool+0x1a8` of that pool:

- bind base == `pool+0x188` (or the bind starts inside the table and covers `[pool+0x188, pool+0x188+4N)`): a V# over
  the table exists. The shader may read it. Then this is class (a) or (b): find the V# builder and announce the
  table (section 1.6).
- bind base is outside the table (the table sits inside a larger or neighbouring bind): class (c). No announce.
  This is the expected result from the static evidence.

### 1.6 Hook proposal (conditional, not to be added unless 1.5 shows a V# over the table)

- Site: `FUN_00109970` loop, after the call `0x109a09 call 0x1018c0`. The next instruction is at **0x109a0e**:
  `mov rax, [r13+0x104a0]` (7 bytes), bytes `49 8b 85 a0 04 01 00`. No RIP-relative operand, no branch target at
  0x109a0e (the loop's branches go to 0x1099f0 and 0x109a17). Unique-pattern check was not run because no hook is
  proposed yet.
- Registers at 0x109a0e: `rbx` = loop element (stride 0x30, callee-saved, survives the call), so the pool is
  `*(u64*)(rbx+0x28)`; `N = *(u64*)(pool+0x1a8)`; table = `*(u64*)(pool+0x188)`. Range to announce:
  `[table, table + 4*N)`, kind Persistent (CPU-written, read by the GPU only if 1.5 says so).
- Why not a Wrap of 0x1018c0: it has a clean prologue, but the table range depends on the pool, and the site after
  the call sees the same pool through `rbx`. A site is less code.
- Cost if added: the whole table is announced once per pool per frame, not per write. That is cheap.

## 2. RIP 0x119e27 (not a particle writer)

- Function: `FUN_00118e10` (0x118e10..0x11b5ce, 10 KB). Its decomp has the strings "Spot light Radius %f ConeAngle
  %f" and "HiRes SpotShadow Mode Error: ..." **O**. Callers: `FUN_00118ca0` (site 0x118d89) and `FUN_0011b660`. **O**
- The instruction `mov [rax+0x80], rcx` (0x119e27) stores the 8-byte tail of a 0x88-byte block (four ymm copies
  from `[rsp+0x1d0..0x250]`, then the qword at `+0x80`). `rax` is `[r14+0x670]` (or `+0x610`). This is a per-light or
  per-shadow-view constant block of the render context. **I**
- Particle relation: none. The function is the spot-light and shadow setup. **O**
- Class: GPU-read per-frame constants (likely class a). **I**. Owner and the GPU consumer are in the lights/shadow
  area; not checked here.
- Hook: no. Route to the lights / shadow specialist with the whole 0x88-byte block, not this one qword.

## 3. RIP 0x11bd99 (not a particle writer)

- Function: `FUN_0011b6d0` (0x11b6d0..0x11c0b0). Called from `FUN_00118ca0` (site 0x118db8), `FUN_0011b5d0`, `FUN_0011b620`,
  `FUN_0011b660`. **O**
- The instruction `vmovups [rax], ymm0` (0x11bd99) with `rax = [entry+0x70]` (bump cursor, `add [entry+0x70], 0x20`
  after each store) is a 32-byte copy of one sorted item into an output stream. The item comes from the input list
  `param_2+0x48..0x50` (32-byte items, sorted by `FUN_00120b40`), in index order from the list at `[rbp-0x70]`
  with a -1 terminator. **O**
- The input list is passed from `FUN_000cdcc0` (per-view draw): `FUN_00118ca0(param_3, param_1+0x4218, 0x2a, ...)`
  at 0xce2be, pass id 0x2a, flag bit 2 clear, so the copy path runs. **O**
- Particle relation: none found. Particle functions have no call to `FUN_00118ca0` or `FUN_0011b6d0` (checked
  the callees of `0x1018c0`, `0x103ad0`, `0x1051f0`, `0x107540`, `0x109ca0`, `0x109970`). **O**. Not checked: which
  producers fill the list at `param_1+0x4218` (could include particle draws submitted through another path). **U**
- Class: generic instance or draw-list records, probably per-frame GPU-read (class a). **I**. The 32-byte item is
  the list record, not particle state.
- Hook: no. Route to the draw-list / instance-writer group.

## 4. Notes for the coordinator

- verify6 counts for 0x101ae4 (4706 consumed, 4792 unannounced) are only a page-overlap signal until section 1.5
  is run. Do not add an announce for it on the current evidence.
- The count ring `pool+0xf8` (GPU-written, CPUMemory per 1.2) is the GPU-read array of consumer `d` in
  impl-particles.md; its announce is a separate matter from the stamp table.
- Existing hook `NATIVE_PARTICLE_SHIFT` (0x101817, particle record writes) is in the same function. It is not
  duplicated here.
