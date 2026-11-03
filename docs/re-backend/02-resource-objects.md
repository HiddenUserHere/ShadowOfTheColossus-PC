# 02 - Engine GPU resource objects: lifecycle and descriptors

Read-only static analysis. Primary source: Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`
(exported from the Ghidra project in `out/re/ghidra`). `tools/re/eboot_re.py` used for strings,
imports, callers and vtable reads. Addresses are eboot VAs. Labels: **observed** (decomp or string,
with address), **inferred**, **unknown**.

## 1. Answer

- **Partly yes for textures.** A `CTexture` object (0x1c0 bytes, vtable `0x15977d0`) owns a 32-byte
  T# at object+0xc4, read raw from the `.ctxr` stream at load (`0x96680`, `0x8de50`). Only its base
  address is written later, on placement (`0x881e60`). Two more 32-byte view blocks (+0xe4, +0x124)
  are built at load (`0x87e5f0`, `0x87dea0`). **observed**
- **Draw-time descriptors are copied, not owned.** The material bind (`0x85f410`) copies per-shader
  descriptor dwords into a per-context user-data array (`ctx+0x3090`) and writes GPU addresses into
  SGPR slots (`0x86d4c0`) on every draw. The owner of the source dwords is **unknown**.
- **Shader and material objects: persistent, GCN binary location unknown.** Shader objects (0x88 bytes,
  factory `0xb0d4a0`) and material objects (0x638 bytes, factory `0xf9390`) are created by the resource
  factory and kept by the engine. Where the shader binary pointer and the material's T# table live is
  not yet found. **inferred / unknown**
- **Texture upload is raw PM4.** Data reaches VRAM by DMA_DATA packets (header `0xc0055000`) that the
  engine writes into its own command buffer (`0x86a700`). No `sceGnm*` copy call is used. **observed**
- **Destruction is per object.** The `CTexture` dtor (`0x985d0`) releases its view blocks and calls
  `sceGnmUnregisterResource` if the object was registered. **observed**
- **Vertex, index and constant buffers: unknown.** The mesh factory `0x1264d0` (4,988 bytes) is
  registered but was not read.

## 2. Resource factory and file types

Loader core (**observed**, Ghidra):
- `ResourceFactory::CreateResource` `0x147160`: checks state (`0x14d210`), reads the loaded buffer
  (`0x14c000` returns the data pointer, `+0x90`, when `+0x80` is set), then calls `0x144880`.
- `0x144880` calls the entry's build function (entry+0x78) with the raw file bytes and a
  `FactoryResourceBuildData`. On failure it logs "Failed to create resource %s" (`0xfa97d1` area).
- `ResourceFactoryResourceLoader::LoadFileInternal` `0x14b980` reads the file in chunks. `0x14aee0`
  opens it, with fallbacks ("Failed to open resource file %s. Will try fallbacks").
- `ResourceFactory::DeleteResource` `0x1478d0` (693 bytes): unlinks the resource from a per-type list
  and releases it. Only partly read.

Factory registration (**observed**): `FUN_00144640(entry, name, build_fn, flags)` followed by
`FUN_001444e0(factories, tag, entry)`.

| Name / tag | Register site | Build function | Object size or note |
|---|---|---|---|
| `Texture`, tag `0x63747872` | `0x112730` (from `0x98a90`) | `0x98aa0` -> `0x96680` | CTexture, 0x1c0 bytes |
| `TextureStreamingChunk`, tag `0x63747863` | `0x112730`, `0x112900` | `0x1129a0` | 0x88 bytes, job-driven upload |
| `ParticleTexture` | `0x10e910` | `0x10e9b0` | 0x68 bytes, atlas region |
| `CPUTexture` | `0xa2ab50` | `0xa2ac00` | not read |
| `Material` | `0xf9270` | `0xf9390` (calls parse `0xf6c90`) | 0x638 bytes |
| `Shader`, `ShaderList` | `0xb0d3e0`, `0xb0e3a0` | `0xb0d4a0` (ctor `0xb0ced0`), `0xb0e440` | Shader 0x88 bytes |
| `Model` | `0x110540` | `0x110630` | not read |
| Mesh, tag `0x344d455348` ("HSEM4") and `0x636d7368` (cmsh) | `0x127860` (from `0x2569d0`) | `0x1264d0` | 4,988 bytes, not read |
| `Skeleton`, `Animation`, `StaticLighting`, `VirtualGeomtrySetup`, ... | various | various | not read |

Startup references (**observed**, `0x143f50`): global resource references created at static init
for `engine-shaders.cslt`, `particle-shaders.cslt`, `lightprobe-shaders.cslt`, `default.ctxr`,
`whiteNoise_msk.ctxr`, `default.cmat` plus six preview materials, `default.cmdl`, `*.cmsh`,
`default.cprt`, `console.cfon`.

File extensions in strings (**observed**): `.ctxr` texture, `.cmat` material, `.cmdl` model,
`.cmsh` mesh, `.cslt` shader library, `.cprt` particles, `.cfon` font, `.cgpr` object prototype.

## 3. Texture object: T# ownership and build

`0x96680` (`FUN_00096680`, 1,629 bytes) builds the CTexture from the file stream. Steps (**observed**):

| Step | Address | Effect |
|---|---|---|
| Allocate 0x1c0-byte object | `0x17070` | CTexture, vtable `0x15977d0` |
| Base ctor | `0x112430` | base vtable `0x1597e30`, flag bytes at +0x61..+0x66 |
| Mip table | `0x8ea20`, `0x8eae0`, `0x8e410`, `0x99140` | vector at +0x80/+0x88, 80-byte entries (stride 0x14 dwords) |
| Read 32 bytes into +0xc4 | `0x8de50(stream, obj+0xc4, 0x20)` | **the T#**, raw from file |
| Mip data placement | `0x12820` (small) or `0x12950` (virtual GPU heap, `DAT_02ed1f60`) | data pointer at +0xb8 |
| Field derivation | `0x880c30`, `0x8829e0` | mip range and flag bits in T# words (**inferred** from bit masks) |
| Base address patch | `0x881e60` (365 bytes) | writes base address >> 8 into T# dword0, keeps other bits |
| View blocks +0xe4, +0x124 | `0x87e5f0` (1,299 bytes), `0x87dea0` | two 32-byte view descriptors (**inferred**: per-view or per-mip) |

Memory family: `0x96680` passes `&DAT_02dd9f84` (GPU family, report 05) for the object. Particle
textures pass `&DAT_02dd9f80` (CPU family, `0x10e9b0`).

The T# is persistent: it lives in the object from load until the dtor. Only the base address field
is rewritten on residency (`0x881e60`). **observed**.

## 4. Texture upload and streaming

- `TextureStreamingChunk::ProcessTextureData` `0x112b80` (419 bytes): kicks a job
  (`JobSystem_KickJobArray` `0x35480`) that runs `ProcessTextureDataInternal` (body `0x112d30`).
  The body calls `0x98ae0` on the texture at chunk+0x78. **observed**
- `0x98ae0` (693 bytes): per-mip records of 0x50 bytes; allocates from the streaming pool (`0x12950`,
  `DAT_02ed1f60`), updates the pool counter at +0x1b0, and checks readiness with `0x14cf00`. Partial
  read. **observed**
- Copy `0x98800` (585 bytes): copies per mip in 256-byte units (`<< 8` offsets), addresses from
  `0x88f000`, base offset from `0x882710`. A non-null command pointer selects DMA (`0x85db40`), otherwise
  a CPU memcpy. **observed**
- `0x85db40` (202 bytes): splits a copy into `0x1ffffc`-byte packets and calls `0x86a700`. **observed**
- `0x86a700` (238 bytes): writes one PM4 packet at the command cursor (`cmd+0x10`, room check against
  `cmd+8`, refill callback at `cmd+0x18`): header `0xc0055000` (DMA_DATA), source and destination
  addresses, byte count in the low 21 bits. **observed**. No `sceGnm*` call is involved.
- Per-frame GPU step `CRenderer::UpdateTextureStreamingGPUData` `0x117930` (CRenderContext) and the
  CComputeContext instance (caller `0x117160`): body not read. **unknown**
- Streaming states are an enum: `ETextureLoadState::Idle`, `AutomaticLoading`, `Unloading_Stage0..3`,
  `Unloading_End` (strings `0xfa8b84`-`0xfa8ca5`). **observed** (names only)
- The upload budget that defers first uploads (report 07) is port-side. The engine-side budget is
  **unknown**.

## 5. Shaders, materials and draw-time binding

Shader factory `0xb0d4a0` allocates 0x88 bytes and calls the ctor `0xb0ced0` (878 bytes). The ctor
reads a count and then a table of 32-byte entries through `0x8df50`. **observed** (count and stride).
What each entry means is **unknown** (candidates: reflection data or per-shader descriptor slots).
`ShaderList` is registered at `0xb0e3a0` (see report 07).

Material factory `0xf9390` allocates 0x638 bytes and calls the parse `0xf6c90` (5,137 bytes). Texture
and shader references are read there (**inferred**). Not decompiled in full.

Draw-time binding (**observed**):

| Function | Role | Evidence |
|---|---|---|
| `0x85f410` Material_BindGraphicsShaders_A | per draw: `GnmShader_SetPs350`/`SetVs` (`0x86c050`/`0x86c170`), or the Update variants; copies `count*4` bytes from `ctx+0x3090` into a bump allocation (`0x8643f0`); for each slot calls `0x86d4c0` (SetUserData: writes a GPU address or dword into an SGPR) | decomp |
| `0x8643f0` | bump allocator for user-data dwords; refill callback in the allocator object | decomp |
| `0x8611a0` | copies 16-byte descriptors (4 dwords) from the caller into `ctx+0x3090 + slot*4` | decomp |
| `0xc7850` | per frame, rotates a 0x828-byte-per-cascade state double buffer (`ctx+0x2a10` and neighbours) | decomp |
| `0x862c50` (1,248 bytes) | draw state flush; no direct caller (indirect) | decomp |

So descriptors are copied per draw from a source array into the context's user-data buffer. GPU
addresses of persistent objects (textures, buffers) go into SGPRs. The source array for a material
is **unknown**.

## 6. Destruction

- `CTexture` dtor `0x985d0` (177 bytes), vtable slot 0 of `0x15977d0`:
  1. releases three view blocks at object+0xa0, +0xa8, +0xb0 via `FUN_00090300(DAT_02ed33f0)`
     (free of a view or descriptor; **inferred**);
  2. if the object holds a GNM registration id (dword at +0x1b8, not -1), calls
     `sceGnmUnregisterResource`;
  3. calls the base dtor `0x1124b0` (which runs `0x112a80`).
  Slot 1 `0x98720` is the deleting dtor (dtor, then free `0x170b0`). **observed**
- Vtable `0x15977d0` (read from eboot): 0 `0x985d0`, 1 `0x98720`, 2 `0x98e60` (empty), 3 `0x98e70`,
  4 and 5 zero, 6 `0x9a9e0` (calls `0x47620`), 7 `0x9a9f0`, 8 `0x13d10`, 9 `0x13d20`. **observed**
  (slot names beyond 0-2 not identified)
- Resource teardown: `ResourceFactory::DeleteResource` `0x1478d0` (partly read).
- Note on the index: the local index reports 0 direct callers for `sceGnmUnregisterResource`, yet the
  decomp of `0x985d0` calls `import_sceGnmUnregisterResource`. The decomp is taken as correct; the index
  misses some import stubs. **observed**

## 7. Vertex, index and constant buffers

- The mesh factory `0x1264d0` (4,988 bytes) is the candidate for vertex and index buffer creation. Not
  read. **unknown**
- Strings: `CMesh::MeshFactory` (`0xfa701e`), `CRenderer::TryCreateMesh` (`0xfa6151`).
- No `sceGnm` buffer-creation import is in the import table (no create-buffer or create-texture call).
  V# would therefore be written inline, as the T# is (**inferred**). **observed** (import list)
- Constant data: draw-time user data only (section 5). A persistent constant-buffer object: **unknown**.

## 8. Imports used for shaders and draws

Present in the import table: `sceGnmSetPsShader350`, `sceGnmUpdatePsShader350`, `sceGnmSetVsShader`,
`sceGnmUpdateVsShader`, `sceGnmSetCsShaderWithModifier`, `sceGnmSetVgtControl`, `sceGnmDrawIndex`,
`sceGnmDrawIndexIndirect`, `sceGnmDispatchDirect`. Absent: `sceGnmGetTextureSizeAndAlign`,
`sceGnmCreateTexture`, `sceGnmCreateBuffer`, any GNF loader. **observed** (import list)

## 9. Answers to the brief

| Question | Answer | Evidence |
|---|---|---|
| Engine objects own stable T#s? | Textures: yes (+0xc4, +0xe4, +0x124) | `0x96680`, `0x881e60` |
| Engine objects own V#s? | Unknown (mesh factory not read) | `0x1264d0` |
| Shader objects persistent? | Yes (0x88 bytes); GCN binary location unknown | `0xb0d4a0`, `0xb0ced0` |
| Materials bind via a T# table? | Partly: dwords copied per draw from an unknown source | `0x85f410`, `0x8611a0` |
| Texture upload path | Engine-written DMA_DATA PM4, driven by a streaming job | `0x112b80`, `0x98800`, `0x85db40`, `0x86a700` |
| Destruction | Dtor releases views and unregisters the GNM resource | `0x985d0` |

## 10. Open items

1. Read `0x1264d0` (mesh factory) for vertex and index buffer creation, and whether V# are stored.
2. Read `0xf6c90` in full: which material fields reference textures and shaders, and how a T# table is built.
3. Find the owner of the 16-byte descriptors that `0x8611a0` copies (callers include the pre-render path `0xca8e5`).
4. Read `0xb0ced0` and the `ShaderList` reader `0xb0e440` to locate the GCN binary pointer.
5. Read `0x117930` (`UpdateTextureStreamingGPUData`) for the per-frame GPU step.
6. Confirm free and placement semantics in `0x90300`, `0x12950`, `0x12820`.
7. Read `ResourceFactory::DeleteResource` (`0x1478d0`) in full: teardown order across the factory lists.
