# Particles: engine side (per-frame flow, effect resource, emitter link)

Read-only static analysis of the EU 1.01 eboot (`out/re/decomp/<hi16>/<addr>.c`, Ghidra export; strings from
`tools/re/eboot_re.py str`). No game dump is in this checkout, so no `.cprt` byte was read. Addresses are eboot VAs.
Labels: **observed** (decomp, string, import name), **inferred** (reasoning from observed code), **unknown**.

## 1. Answer in brief

- Each particle effect is one **`.cprt` resource** (FourCC `cprt`, factory name `ParticleSystem`, path
  `$/coredata/enginesupport/particles/_ps4/<name>.cprt`). The engine default is `default.cprt`. **observed**
- The `.cprt` file is a **relocatable table tree** (rel-pointers, same convention as `.cslt`). It holds, per effect:
  an effect header (emitter count, per-particle payload size, max particles, effect type index), a table of
  **emitters** (variable-size records), and for each emitter: its name, a **per-emitter constant blob**, a
  per-particle payload stride, the **names of three compute shaders** (`.csdr`), the parent index, and four typed
  sub-blocks. **observed (parser FUN_000fd250, consumers FUN_000fffa0, FUN_000fef40)**
- The effect's **logic is not held as an interpreted graph** in anything the CPU reads. The CPU only copies the
  constant blob into a buffer, counts spawns, and dispatches. Control flow of spawn, update and emit is in the
  **compiled GCN programs** (the three `.csdr` per emitter). **observed (no interpreter in UpdateSimulation) /
  inferred (the logic lives in the shaders)**
- Whether the constant blob and the four typed sub-blocks already encode the modules (curves, shapes, velocities)
  as data is **unknown**: their schema is not in the code, and only the shader that reads them would show it. The
  dump is needed to decide. **unknown**
- The link from an effect to its programs is **data**: each emitter record holds three rel-string names, resolved
  through the resource factory by name to `.csdr` objects. The port can enumerate every emitter's three programs
  from the `.cprt` alone, without running GCN. **observed (resolution) / inferred (name roles)**

## 2. Per-frame flow

### 2a. Entry points and job order (observed)

- Frame job `FUN_000d0040` (`out/re/decomp/000d/000d0040.c`):
  - `FUN_00107540` (ParticleRenderer::BeginFrame, 0x107540) when `*(this+0xdc0) != 0`. Labels: `clear emitter-type
    particle count`, `init new system type`.
  - `UpdateSimulationJob` (job name string 0xfa15db), function `FUN_001074b0` picks the job queue value
    (returns 2, or 0x604 when `*(job+0x48)+0x7d` is set and `+0x8c == 3`). **inferred**: queue or pipe selection for
    the simulation job; not traced further.
  - `PrepForRenderingJob` (name 0xfa1610, function `FUN_00106950`, label `ParticleRenderer::PrepForRendering`).
  - Render passes (`RenderTranslucent`, `RenderDistortion`, `RenderFog`, `RenderModels`, ... strings 0xfa5af6..0xfa5bd8).
- `ParticleRenderer::UpdateSimulation` exists twice: `0x103ad0` (5,874 bytes) and `0x1051f0` (5,892 bytes). Same
  labels and phases. `0x1051f0` has no direct caller in the Ghidra headers (reached through a pointer, or unused).
  **observed**. The analysis below is for `0x103ad0`; `0x1051f0` was not diffed in detail. **inferred** same logic.

### 2b. Inside `UpdateSimulation` (0x103ad0), in order (observed: marker strings and call order)

`param_1` = ParticleRenderer (manager), `param_2` = command context (GPU marker and command ring), `param_3` = GPU
dispatch context, `param_4` = the particle pool (system list at `param_4+0x10498` .. `+0x104a0`, stride 0x30, system
pointer at `+0x28`).

| # | Marker (string) | Work | Dispatch | Buffers / counters touched |
|---|---|---|---|---|
| 0 | `ParticleUpdate` | scope | - | - |
| 1 | `clear global counters` | `FUN_0010bfd0` (357 bytes): one 0x20-byte record (3 dwords and a 16-byte descriptor), one bind, one dispatch | `sceGnmDispatchDirect`, groups `ceil(n/64)` x 1 x 1 | `manager+0x1508` ring (record copied per call) |
| 2 | `clear per-emitter-type particle counters (prev)` / `(current)` | same helper | same | `pool+0x340` counts, frame-parity table `pool+0x338` (two slots of 0x28) |
| 3 | `clear per-emitter-type spawn request counters` | same helper | same | `manager+0x1760` (count `+0x1768`) |
| 4 | `clear per-model-emitter-type model draw counters` | same helper | same | `manager+0x1788` (count `+0x1790`) |
| 5 | `update systems` | scope; then loop over systems (see 2c) | - | - |
| 6 | `update parent emitters`, then `update child emitters` | two passes, index `uVar20` = 0 then 1 (2d) | - | - |

The clear and prep helpers bind a **global** program, not one per effect: `FUN_0010bfd0` takes
`*(manager+0x38)+0x68`, then resource `+0x150`, then `+0x60` (the blob pointer of the `.csdr`). `FUN_0010c140` takes the
same object and resource `+0x170`. **observed**. These two programs (clear, prep-indirect) are shared by all effects;
their names are not known (the object at `manager+0x38` is probably a shader collection; **inferred**).

### 2c. Per system, before the passes (observed unless marked)

For each system `sys` in the pool list:
- `FUN_000f56a0`, `FUN_00095580`, `FUN_0014d7b0(sys+0x28)`: resolve the effect resource (`cprt`) of the system.
- `FUN_001013f0(sys, camera, flag)` (803 bytes, CPU): walks the live-particle bitset (`sys+0x198`, count `sys+0x1a0`,
  capacity `sys+0x1a8`). For each live particle it computes a distance from the camera (`camera+0xb8..0xc0`) and
  stores a 0..1 fade into `particle+0xd4`. It decrements the live count `sys+0x1bc` when the fade is 0 or less.
  **observed (code)**. CPU-side culling by distance: **inferred**.
- Clear of per-system counters, `clear per-system-instance particle counters` (`sys+0xd0`, count `sys+0xd8`).
- `FUN_00101720(sys, ...)` (402 bytes, CPU): walks the live bitset and copies each live particle record (stride
  `sys+0x74`) from `sys+0x178` into the destination, building a compacted index list. **observed (memcpy)**. Whether
  the source is a CPU mirror or a readback of GPU-written data is **unknown**.
- `FUN_000ffe60(emitter, pool+0x2e0, parity, sys+0x1bc)` for each emitter (318 bytes, CPU). It allocates the
  emitter's per-frame buffers, sized `(spawn count) x (record+0x1c)` where the count is `sys+0x1bc` (see 4b). **observed
  (sizes and calls)**.

### 2d. Parent and child passes, per emitter (observed)

For `uVar20` in {0 = parent, 1 = child} (the pass chooses emitters by their parent index, see 3b):

1. **enqueue spawn requests** (marker). For each matching emitter `E` (entry size 0x168, `sys+0x170`):
   - Marker label = the emitter's name (rel string at `record+0x24`, see 3b). **observed**
   - Bind program **ref 1** (the `.csdr` named at `record+0x28`) and dispatch
     `sceGnmDispatchDirect(1, ceil(spawnCount / G), 1)` with `G = record+0x10` and `spawnCount = sys+0x1b8`.
     **observed (dispatch in FUN_00883870 at 0x103ad0 line ~267; ceil from `uVar43 = spawnCount-1+G`, `/G`)**.
   - Writes a **spawn request** of 0x70 bytes into a ring slice (`FUN_0008c0d0`). Fields (observed, lines 226-246):
     `+0x00 u32 spawnCount` (`sys+0x1b8`), `+0x04 u32 random seed` (`FUN_00158370(&DAT_02edf010)`), `+0x10` two
     16-byte buffer descriptors from `sys+0x80` and `sys+0xb0`, `+0x20` from `emitter+0x38` (16 bytes), `+0x30` from the
     pool-level descriptor `manager+0x1878`, `+0x40` the first 16 bytes of the emitter, `+0x50` the spawn-request
     counter descriptor `manager+0x1760`. The record is consumed by the GPU. **observed (writes) / inferred (consumer)**
   - Writes a 16-byte pair and a `u32` into `param_2+0xb0` and `+0xb8` (the command's data pointers), and a 0x28-byte
     record via `FUN_00012820` (GPU family heap) for the command.
   - Optionally `FUN_0085e5d0` writes per-dispatch user data (a copy of a 0x... block from `+0x98` into a frame ring
     `FUN_008643f0`, then SGPR-pointer updates). **inferred (from the calls)**.
2. `FUN_00884d40` = `sceGnmComputeWaitOnAddress` (wait for the counters). Observed as an import name in the callee
   list.
3. **prep process spawn requests** (marker). `FUN_0010c140` per emitter:
   - Writes an **indirect-args record** of 0x50 bytes at `manager+0x1648` (`FUN_0008c0d0`): u32 values from the emitter
     (`+0x00`, `+0x04`, `+0x08`, `+0x0c` from the record `+0x10` and the pass data `+0xd0`), then three 16-byte
     descriptors (buffers of the system: `sys+0x80`, `sys+0xa8`, `sys+0xd0`-like). **observed (writes)**.
   - Binds the **global prep program** (`manager+0x38` path, resource `+0x170`) and dispatches `(1,1,1)`.
     **observed**. So the prep step is a one-thread program that formats indirect arguments for the next step.
     **inferred**.
4. **process spawn requests** (marker). `FUN_0016edc0` binds **ref 2** (the `.csdr` at `record+0x2c`) and the spawn
   pass runs through `FUN_0010c2e0` (498 bytes), which picks a pass slot of the resource (`FUN_00125500`,
   `FUN_00123da0`, `FUN_00193560`, `FUN_000b2d20`) and returns a pointer into the command ring (`+0x238`). **observed
   (calls)**. Binding of ref 2 to this step is observed (`FUN_000fa1a0(out+0x38)`, line 340-349). **observed**.
5. `FUN_0016ed40(param_3,0)`: scope or barrier (not identified). `FUN_0016b890`: scope end.
6. **update preexisting** (marker, after `prep update preexisting`, which is `FUN_0010c140` again, lines 533-544). Binds
   **ref 3** (the `.csdr` at `record+0x30`, lines 572-578) and dispatches through `FUN_0010c2e0` and
   `FUN_0085ecd0`/`FUN_0085e5d0`. **observed**.
7. `FUN_0016b900` closes each marker.

After both passes, `PrepForRendering` (0x106950) runs `sort draw-sort records`, `sort ribbon-sort records` and
`connect ribbons` (labels), with `FUN_0010c140` (prep, 5 args) and `FUN_0085e5d0`. **observed**

### 2e. Dispatch count per frame

Per system per pass: one spawn dispatch, one prep, one process, one prep, one update (per emitter with a matching
parent flag). Plus the four global clears and the per-system clear. Exact counts depend on the live emitters
(**unknown** without a trace). The `FUN_00883870` calls (`sceGnmDispatchDirect`, 9 DWORD packets) are the dispatches.
**observed (import)**

## 3. The effect resource (`.cprt`)

### 3a. Registration and loading (observed)

- Registration `FUN_000fde80` (0xfde80, 160 bytes, from `FUN_002569d0`, the area loader): `FUN_00144750(...,
  "ParticleSystem", build = FUN_000fdf30, delete = FUN_000fdfd0, 0)`, then `FUN_001444e0(factories, 0x63707274,
  entry)`. FourCC `0x63707274` = `cprt`. Name suffix string `_ps4` (0xfa549e). **observed**
- Build `FUN_000fdf30` (144 bytes): `FUN_00017070(0x70)` makes the resource object, `FUN_0008df50(buffer)` reads the
  file, `FUN_000fd250(obj, &file)` parses it. **observed**
- Parse `FUN_000fd250` (0xfd250, 1,787 bytes). Object layout (observed):

```
resource object (0x70)
  +0x00  vtable 0x1597cc8
  +0x60  file buffer (file root, see 3b)            [param_1[0xc]]
  +0x68  record array: u64 count, then count records of 0x188 bytes   [param_1[0xd]]
```

- Record (0x188 = 392 bytes = 7 slots of 0x38, observed): `+0x00` ref slot (name hash + name + resolved ref),
  `+0x38` ref slot, `+0x70` ref slot, `+0xa8` typed block A, `+0xe0` typed block B, `+0x118` typed block C,
  `+0x150` typed block D. Each slot has a `+0x28` (or `+0x30`) resolved reference, used later by the emitter.
  The three ref slots are filled by `FUN_000fa1a0` (resolve by name through the factory). The typed blocks are
  read by `FUN_000f4b10`, `FUN_000f4720`, `FUN_000f4a80`, `FUN_000bcae0` and checked by `FUN_000f4e40`,
  `FUN_000dc2f0`, `FUN_000b2d20`, `FUN_000a8700`. These loaders are shared by other resource types; their schemas were not
  decoded. **observed (calls) / unknown (schema)**

### 3b. File layout (from the parser and consumers, observed)

File root (bytes at resource `+0x60`):

```
root+0x00  u32  (not read by code; version or tag, unknown)
root+0x04  u32  emitter count N            -> system+0x68 (FUN_000fffa0)
root+0x08  u32  per-particle payload size  -> system stride = payload + 0xd8 (system+0x74; FUN_000fffa0, FUN_00100b80)
root+0x0c  u32  count of the sorted key table at root+0x6c   (bsearch in FUN_000fffa0)
root+0x10  u32  max particles              -> system+0x6c    (FUN_000fffa0)
root+0x58  u32  effect type index T        -> global tables pool+0x104f8 and pool+0x104ec, stride 0x18 (FUN_00100b80)
root+0x6c  rel  sorted u32 key table (key from FUN_000fb450)
root+0x70  rel  8-byte value table; u32 at entry i goes to system+0x78  (FUN_000fffa0)
root+0x74+4i  rel  emitter record i (file form, variable size)
```

File emitter record `i` at `R = (root+0x74+4i) + rel_i` (observed, FUN_000fd250 and FUN_000fef40):

```
R+0x04  u32  constant blob size S          (FUN_000fef40: copied verbatim, memcpy S bytes)
R+0x08  u32  per-emitter particle payload stride (FUN_000fef40: buffer stride at emitter+0x118)
R+0x0c  u32  dispatch parameter (x side)   (UpdateSimulation, uVar39)
R+0x10  u32  dispatch parameter G          (UpdateSimulation, uVar6; y group size, spawn count / G)
R+0x14  rel  constant blob (S bytes)       (copied into emitter+0x48; buffer emitter+0x50, stride round16(S))
R+0x1c  u32  spawn multiplier              (FUN_000ffe60 / UpdateSimulation: count = sys+0x1bc * this)
R+0x20  s32  parent emitter index, -1 = none    (FUN_000fffa0 `*(R+0x20) < 0`, UpdateSimulation flag)
R+0x24  rel  emitter name (C string)       (GPU marker labels in UpdateSimulation)
R+0x28  rel  resource name 1 (.csdr)       (ref 1, enqueue/spawn; required)
R+0x2c  rel  resource name 2 (.csdr)       (ref 2, process spawn; required)
R+0x30  rel  resource name 3 (.csdr)       (ref 3, update preexisting; required)
R+0x34  rel  typed block A                 (FUN_000ff430, stored at emitter+0xa8)
R+0x38  rel  typed block B                 (mesh emission; FUN_000fe320, stored at out+0xe0; its +0x28 is a mesh ref)
R+0x3c  rel  typed block C                 (stored at out+0x118)
R+0x40  rel  typed block D                 (stored at out+0x150; checked by FUN_000a8700)
```

Notes:
- `FUN_000fef40` returns false (emitter invalid) unless all three resource names resolve (`FUN_0014d7b0` non-null).
  **observed**. So each emitter has exactly three named shaders.
- Typed block B is used for **mesh emitters**: when its mesh ref is non-null the emitter uses the mesh path
  (`FUN_000ff260` = `ParticleWorld::AllocModelEmitterCounter`, `FUN_000fe320`). The check `FUN_000fe240`/`FUN_000fe320`
  emits the message "Mesh: %s, is using material: %s, that is not tagged as InstanceType: Particle". **observed**
  Otherwise the emitter uses `FUN_000ff430` and `FUN_000ff7c0` (`ParticleWorld::AllocEmitterCounter`, string 0xfa5882).
  **observed (strings) / inferred (which alloc is which)**.
- Strings at 0xfa54a3 and 0xfa5441 show `bpe::renderer::MeshEmissionData::CheckForSetupErrors`, i.e. the mesh emission
  block is a typed data class with its own validation. **observed**.

### 3c. Does the resource hold the logic as data? (answer)

What is data (observed):
- the emitter list and its parent tree;
- the names of the three shaders of each emitter;
- a verbatim per-emitter constant blob (size S), copied to a GPU constant buffer;
- the per-particle payload size (one per effect and one per emitter);
- four typed sub-blocks per emitter, one of which is the mesh emission source;
- the effect name and the global type index.

What is not visible in the code:
- the schema of the constant blob. The CPU never reads its fields; only the shaders do. Curves, gradients and shape
  parameters, if any, are inside this blob or the typed blocks. **unknown**.
- the schema of the typed blocks A, C and D. Their loaders are shared with other resource types. **unknown**.

What the logic is (inferred): the control flow of spawn, update and emit (what runs per particle, branches, math) is in
the three compiled programs per emitter. The CPU does not interpret modules. So the effect is not a node graph that
the engine walks at run time. The shaders are the logic. A generic interpreter would need the blob and typed-block
schema, which the shaders define. Decoding those schemas from the GCN code (P2) is the way to know whether a generic
spawn/update program can cover them.

## 4. The link from an effect to its programs

### 4a. Names to programs (observed)

- Emitter `E` (CPU, 0x168 bytes, `sys+0x170 + 0x168*i`), set by `FUN_000fef40`:
  - `E+0x10` = the file record `R` (pointer, `param_2`);
  - `E+0x18` = the output record (0x188, the resource side; `param_3`);
  - `E+0x20` = parent index state; `E+0x48` = constant blob copy; `E+0x50` = constant buffer; `E+0x78`, `E+0xa0`,
    `E+0x118`, `E+0x140` = buffers and counters (created by `FUN_0008be60`).
- Output record slots: `out+0x00` (name 1), `out+0x38` (name 2), `out+0x70` (name 3). Each slot's resolved `.csdr`
  object (`FUN_0014d7b0(slot+0x28 / +0x60 / +0x98)`) gives the program: `*(Shader+0x60)` is the blob header of the
  compute program, passed to `FUN_0016edc0` (bind). **observed**
- Each `.csdr` object is the loaded `Shader` resource (`rdsc`, 0x88 bytes) described in `shader-collections.md` (section
  2b). The name is resolved through `FUN_000fa1a0` (factory lookup) with the build suffix (`_shipping.csdr`).
  **observed**

### 4b. Which name is which pass (inferred from the call sites)

| Name | Record offset | Used in phase | Program kind (inferred) |
|---|---|---|---|
| 1 | `R+0x28` | enqueue spawn requests (2d.1) | spawn (writes spawn requests) |
| 2 | `R+0x2c` | process spawn requests (2d.4) | emit (creates particles from requests) |
| 3 | `R+0x30` | update preexisting (2d.6) | update (per-particle step) |

The predict table counts per kind (`compute-duplicates.md` section 3): spawn 1,470 programs, update 1,437, emit 1,138.
The emit programs are the smallest and most often identical (193 identical shapes), which fits name 2 being a small,
mostly generic step. This is consistent with the mapping but **not proven**: the names are not read from the file (no
dump), and the stems in `predict_table.tsv` are the `.csdr` stems, which match `emitter_<n>_<kind>`. The index `<n>` is
either the emitter index in the effect or the effect's type index; **unknown**.

### 4c. Global shared programs (observed)

- Clear program (`manager+0x38 -> +0x68 -> +0x150`) and prep-indirect program (`... -> +0x170`). They are used by every
  effect. These two are candidates for the generic spawn/update set: the port already has them as shared programs.

## 5. CPU-written data per emitter per frame (observed unless marked)

| Data | Written by | Where | Notes |
|---|---|---|---|
| spawn count of the system | `FUN_00101720` / `FUN_001013f0` (live count) and `FUN_00100b80` (`sys+0x1b8 += 1` per new particle, `+0x1bc`) | `sys+0x1b8`, `sys+0x1bc` | per system, not per emitter |
| per-frame spawn multiplier | `FUN_000ffe60` | emitter buffer sizes | uses record `+0x1c` |
| random seed | `FUN_00158370(&DAT_02edf010)` | spawn request `+0x04`, particle `+0xd0` | CPU RNG |
| buffer descriptors (V#) | `FUN_0008be60` / `FUN_0008c0d0` | `sys+0x80`, `+0xa8`, `+0xd0`, `+0xf8`, `+0x120`, `+0x148` and per emitter | |
| emitter constant blob | copy at create (`FUN_000fef40`) | `E+0x50` | static; not per frame |
| time or time scale | `FUN_001013f0` reads `pool+0x104ec + type*0x18` (flag) and `pool+0x104f8` in `FUN_00100b80` | per type | the cvar `particleTimeScale` (0xf9aee7) is read in `RegisterXprCvars`; the link to the table is **inferred** |
| camera position | `FUN_001013f0` reads `camera+0xb8..0xc0` | distance fade | |
| particle transform and state | `FUN_00100b80` initialises a record: `+0x00..0x5c` identity-like floats, `+0xc8` flags, `+0xcc = -1`, `+0xd0` seed, `+0xd4 = 1.0`, `+0xd8..` the constant payload copy (size `root+0x08`) | `sys+0x178` | CPU-initialised, GPU-updated (**inferred** for the GPU part) |

Particle record (stride `sys+0x74`, observed from `FUN_00100b80`, `FUN_001013f0`, `FUN_00101720`):
- `+0x00..+0x2f` 3x4 transform; the translation is at `+0x0c`, `+0x1c`, `+0x2c` (**inferred**, from the distance code)
- `+0x60`, `+0x80`, `+0xa0`: three vec4 (**unknown** meaning)
- `+0xc0` u32 type or emitter index (`0x23` written at `+0xc8` in init; **unknown**)
- `+0xcc` u32 link, `-1` at init
- `+0xd0` u32 seed, `+0xd4` float fade (0..1)
- `+0xd8` payload (`root+0x08` bytes)

## 6. Answers to the four questions

1. **Per-frame flow.** Section 2. Order per emitter: enqueue (ref 1, writes requests) -> wait -> prep (global, 1x1x1)
   -> process (ref 2) -> prep (global) -> update (ref 3), for parent then child. Clears first. Prep and clear are global
   programs. Dispatch buffers: spawn request ring (`0x70`), indirect args ring `manager+0x1648` (`0x50`), counters at
   `manager+0x1760`, `+0x1788`, `sys+0xd0`, per emitter `E+0x50/0x78/0xa0/0x118/0x140`.
2. **Parameters reaching the compute program.** Per dispatch: user data and the copied constant block (`FUN_0085e5d0`
   copies `param+0x98` into a frame ring, then sets SGPR pointers). Per emitter: the constant blob (static, in a buffer).
   Per system and per frame: spawn count, seed, buffer descriptors, transforms. Curves and gradients: **unknown**.
3. **Effect resource and its data.** Section 3. The effect is `cprt` (`ParticleSystem`), a relocatable table tree of
   emitters, each with a constant blob, a payload stride, three shader names and four typed sub-blocks. The logic is not
   held as an interpreted graph (3c).
4. **Link to programs.** Section 4. Three rel-string names per emitter, resolved by name through the factory to `.csdr`
   (`FUN_000fa1a0`). Required; the emitter is rejected otherwise.

**Plain answer for the native design.** The shipped game holds, as data: the emitter graph, names of the three programs,
constant blobs and typed blocks. The data that a generic interpreter needs (the modules and their parameters) exists
only in the form that the compiled shaders read: the constant blob and typed blocks, whose schema is not decoded. The
logic (which operations, in which order) is only in the GCN programs. A native port therefore needs either:
(a) the schema of blob and typed blocks, obtained by decompiling the GCN spawn/update/emit programs and mapping the
constant-buffer offsets they read (P2's work); or (b) a runtime capture of the blobs from a real `.cprt` (the port can
read them in `FUN_000fd250` / `FUN_000fef40` at run time) to see whether the blob already holds a full description.

## 7. Port use (run time)

- Read the effect description when a `cprt` loads: hook `FUN_000fd250` (parse done, records filled) or `FUN_000fef40`
  (per emitter, file record in `param_2`, output record in `param_3`). The emitter names (`R+0x24`), the three shader
  names (`R+0x28..0x30`), the parent index (`R+0x20`), the blob (`R+0x14`, size `R+0x04`) and the typed blocks are all
  readable there. This needs no change to draws. **inferred (design)**
- Read the system state when an instance is created: `FUN_00100b80` (`ParticleSystemPool::CreateInstance`) and
  `FUN_000fffa0` (instance init). The effect's emitter list is `sys+0x170` (count `sys+0x68`, stride 0x168).
- Shared programs (clear, prep-indirect) are available once the first `UpdateSimulation` has run, via `manager+0x38`.
- For a native generic spawn/update, the port would need the per-emitter three names and blobs (available above), and
  the schema of the blobs (missing). Only the blob bytes and a decode of the shader reads can give it.

## 8. Open questions and next steps

1. Dump one `.cprt` (for example `default.cprt`, or an emitter-heavy area's effect) and dump the constant blob bytes
   for its emitters. Compare the bytes with the constant-buffer offsets read by the three programs of that emitter.
   Needs the game dump (`SOTC_GAME_DIR`), which this checkout does not have.
2. Schema of typed blocks A, C and D (`FUN_000f4b10`, `FUN_000f4a80`, `FUN_000bcae0`, and checks `FUN_000f4e40`,
   `FUN_000b2d20`, `FUN_000a8700`). Their names are shared with other resource types; find the one type that matches
   the particle block.
3. Confirm the name roles (section 4b) by reading the `.csdr` for one emitter (ref 1 should contain the spawn writes,
   ref 3 the per-particle update). P2 can do this from the GCN code.
4. Diff `0x1051f0` and `0x103ad0` (which one is used).
5. Identify `FUN_0016edc0`, `FUN_0016ee80`, `FUN_0016ed40` (bind and barrier helpers) and `FUN_00125500` / `FUN_00193560`
   (the pass-slot chooser in `process spawn requests`).
6. The index `n` of `emitter_<n>_<kind>`: emitter index or effect type index (root `+0x58`).

## 9. Evidence list

| Item | Address | Source |
|---|---|---|
| Frame job calls | 0xd0040 | `out/re/decomp/000d/000d0040.c` |
| UpdateSimulation (phases) | 0x103ad0 | `out/re/decomp/0010/00103ad0.c` |
| UpdateSimulation second copy | 0x1051f0 | `out/re/decomp/0010/001051f0.c` |
| Job queue value | 0x1074b0 | `out/re/decomp/0010/001074b0.c` |
| BeginFrame | 0x107540 | `out/re/decomp/0010/00107540.c` |
| PrepForRendering | 0x106950 | `out/re/decomp/0010/00106950.c` |
| Counter clear helper | 0x10bfd0 | `out/re/decomp/0010/0010bfd0.c` |
| Indirect-args prep helper | 0x10c140 | `out/re/decomp/0010/0010c140.c` |
| Process helper | 0x10c2e0 | `out/re/decomp/0010/0010c2e0.c` |
| Per-instance CPU update | 0x1013f0 | `out/re/decomp/0010/001013f0.c` |
| Compacting copy | 0x101720 | `out/re/decomp/0010/00101720.c` |
| Per-emitter buffers | 0xffe60 | `out/re/decomp/000f/000ffe60.c` |
| Instance creation | 0x100b80, 0x102590 | `out/re/decomp/0010/00100b80.c`, `00102590.c` |
| System init | 0xfffa0 | `out/re/decomp/000f/000fffa0.c` |
| Emitter init | 0xfef40 | `out/re/decomp/000f/000fef40.c` |
| Mesh emission setup | 0xfe320, 0xfe240 | `out/re/decomp/000f/000fe320.c`, `000fe240.c` |
| cprt registration | 0xfde80 | `out/re/decomp/000f/000fde80.c` |
| cprt build | 0xfdf30 | `out/re/decomp/000f/000fdf30.c` |
| cprt parser | 0xfd250 | `out/re/decomp/000f/000fd250.c` |
| dispatch (sceGnmDispatchDirect) | 0x883870 | `out/re/decomp/0088/00883870.c` |
| wait (sceGnmComputeWaitOnAddress) | 0x884d40 | `out/re/decomp/0088/00884d40.c` |
| user-data writer | 0x85e5d0, 0x85ecd0 | `out/re/decomp/0085/0085e5d0.c`, `0085ecd0.c` |
| Strings: effect class | 0xfa548f (`ParticleSystem`), 0xfa549e (`_ps4`), 0xfa54a3 (`MeshEmissionData::CheckForSetupErrors`) | `eboot_re.py str` |
| Strings: default effect path | 0xfa91a1, 0xfd6e27, 0xfddf7c, 0xfe34b7 (`$/coredata/enginesupport/particles/_ps4/default.cprt`) | `eboot_re.py str` |
| Strings: phase labels | 0xfa5a01..0xfa5da9 | `eboot_re.py str` |
| Alloc strings | 0xfa5882 (`AllocEmitterCounter`), 0xfa58ea (`AllocModelEmitterCounter`) | `eboot_re.py str` |
