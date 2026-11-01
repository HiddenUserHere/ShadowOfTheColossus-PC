# 03 - Resource blobs: does the GPU read decoded data in place?

Read-only static analysis of `out/eboot.elf` (Ghidra output in `out/re/decomp`, `tools/re/eboot_re.py`).
Labels: **observed** (code/disassembly with address), **inferred**, **unknown**. Decompile line numbers
refer to `out/re/decomp/<bank>/<addr>.c`.

## 1. Answer

- **Not in place.** Every GPU-bound byte is copied from the decoded blob into a GPUMemory-family
  allocation, and the V# or T# base points at the copy. **observed** (0x8be60, 0x96680).
- **The blob is CPU-side and transient.** It lives in heap index 1 (CPUMemory) and is freed when the
  factory's stream object is destroyed at the end of CreateResource. **observed** (0x8db10, 0x8dc30).
- **Mesh:** vertex and index blocks are GPU copies; small arrays are CPU copies. **observed.**
- **Model:** no GPU allocation and no bulk copy. CPU metadata only. **observed.**
- **Non-streamed texture:** the T# base is the GPU copy, set once at build. **observed.**
- **No in-place patching seen.** The blob is only read after the fill; no relocation or fixup
  writes were found. **observed (absence checked in the mesh, texture and model factories).**
- **Consequence for GPU_STATIC:** these types can be tracked from build-time GPU allocations, and
  faults should be rare after build. **inferred.** Texture streaming (report 11) and per-frame
  writers are separate and not covered here.
- **Corrections to the brief:**
  - 0x148d14 is the loader's job call (CreateResource). The per-type build is the function pointer at
    entry+0x78, called from 0x144880. **observed.**
  - The alignment passed to 0x12820 is entry+0x54. Registration (0x144640) sets it to 8 for every type.
    The "resource's own alignment" in the brief is **not confirmed**. **observed.**

## 2. Type registry (observed)

`ResourceFactory` registers each type through `sub_1444e0(registry, FourCC, descriptor)`. The
descriptor is built by `sub_144640(desc, name, build_fn, flags)`, which stores the name at +0x68,
the build function at **+0x78**, flags at +0x70, and alignment 8 at +0x54. The FourCC is the `esi`
value (the table prints the little-endian bytes).

| Type | FourCC (esi) | Build fn | Registration site |
|---|---|---|---|
| Mesh ("Mesh") | 0x636d7368 ("hsmc") | 0x1264d0 `CMesh::MeshFactory` | thunk 0x127860, called from 0x2569d0 at 0x256abb |
| Model | 0x636d646c ("ldmc") | 0x110630 | 0x110540 |
| Texture | 0x63747872 ("rxtc") | 0x98aa0 (calls 0x96680) | 0x112730, called from 0x98a90 |
| TextureStreamingChunk | 0x63747863 ("cxtc") | 0x1129a0 (0x88-byte object) | 0x112730 |
| ParticleTexture | 0x63707478 ("xtpc") | 0x10e9b0 (calls 0x96680) | 0x10e910 |
| Material | 0x636d6174 ("tamc") | 0xf9390 | 0xf9270 |
| Shader | 0x63736472 ("rdsc") | 0xb0d4a0 | 0xb0d3e0 |

Load path (observed unless marked):
1. CreateResource 0x147160 runs as the loader job (pointer at data 0x15feb00; job call at 0x148d14).
2. It calls 0x144880, which calls `[entry+0x78](&{handle, stream, size}, &out_object)`.
3. The stream is built by 0x4d470 (vtable set, then 0x8db10 base ctor).
4. The out object is published by 0x14d670, which writes state 0xb at `[res+0x98]` and the object at `[res+0x20]`.

**Unknown:** how the file record's type word selects the entry, and whether `PrepareForLoad`'s entry
is the registry descriptor or a per-resource copy. The field layout matches, but the copy was not traced.

## 3. Blob lifetime (observed)

| Step | Site | Detail |
|---|---|---|
| Read buffer | LoadFileInternal 0x14b980 (line 119); PrepareForLoad 0x14aee0 (line 119) | `0x12820(size, heap=1, align=entry+0x54, 0)`. Heap 1 is CPUMemory (`DAT_02dd9f80`, set at 0x13380). |
| Decompress target | CreateResource 0x147160 | If `entry+0x70 & 8`, a second heap-1 buffer is allocated and `0x1aa2a0` writes into it. Input and output are different buffers. |
| Stream | 0x8db10 (via 0x4d470) | `+0x120` = blob pointer, `+0x118`/`+0x110` = size, `+0x108` cursor, `+0x110` end, `+0x128` = owned (set by `r8 == 1`). |
| Ownership | 0x14c020 | Zeroes the loader's buffer fields, so the stream owns the buffer. CreateResource passes owned = 1 in both branches. |
| Free | 0x8dc30 | If owned, `0x12af0(+0x120)` frees the blob. Also frees a side list (offsets 0x138-0x140) of fallback copies. |

Stream reads (`0x8eae0`, `0x8de50`, `0x8e740`, `0x8dd70`) only read `+0x120`. Writes go to
destinations, never back into the blob. **observed.**

## 4. Mesh (question 1)

The mesh factory (0x1264d0) parses the blob sequentially and builds C++ objects. Its GPU buffers:

- **Vertex block:** `0x8e0d0` returns a zero-copy pointer into the stream when aligned and contained,
  otherwise a heap-1 copy. Then `0x8be60(obj+0x20, kind=6, stride, count, flags=0, src, 0)` (decomp
  0012/001264d0.c:557-559). **observed.**
- **Second GPU buffer:** `0x8be60(..., kind 0x18 or 0x19, flags=0, src)` (decomp line 669). **observed.**
- **0x8be60 behaviour:** with flags bit 0 clear it allocates `0x12820(size, DAT_02dd9f84, align 0x10, 0)`
  (GPUMemory, kind 2, created at 0x110a0) and `memcpy`s the source into it. Bit 0 set selects
  CPUMemory instead. **observed.**
- **Descriptor:** `0x8652d0` writes the V# words from the new base: word0 = low 32 bits of base,
  word1 = high bits and stride. The descriptor base is the copy, not the blob. **observed.**
- **CPU arrays:** `0x17090` (heap 1) allocations filled by `0x8de50`, e.g. decomp lines 554, 633, 662.
  These are CPU_ONLY. **observed.**

Answer: the GPU never reads the blob. It reads the GPUMemory copy through the V# written at build.

## 5. Texture (question 2)

Build fn 0x98aa0 calls `0x96680(stream, size, obj, &DAT_02dd9f84)`:

- **Single block:** `0x12820(size, GPUMemory, align 0x100, 0)`, then `0x8de50` copies the texels from
  the stream into it (decomp 0009/00096680.c near `uVar9 = FUN_00012820(param_2, *param_4, 0x100, 0)`).
  **observed.**
- **Multi-part (mip branch):** `0x12950(0, size, 1, DAT_02ed1f60, 0x100)`. DAT_02ed1f60 is the family
  from 0x8c370 (`DAT_02ed3350`). Its name is not in the eboot strings; the brief's "VirtualMemoryGPU"
  is **unverified** here. **observed (heap), unknown (name).**
- **T# base:** `0x881e60(desc, base >> 8)` at 0x96b8b. The base is the GPU copy. **observed.**

Streamed textures reach their texel data through a separate path (report 11, memcpy at 0x98d67 in
sub_98ae0). Whether a streamed texture passes through 0x96680 with header-only data is **unknown**.

## 6. Rewrites after build (question 3)

- **Relocation or fixup:** none found. The factories read the stream and build objects. The only
  writes to the blob are the reader and decompressor fills before the build. **observed (absence).**
- **Decompression:** output goes to a new buffer. It is not in place. **observed.**
- **Stream pointers:** zero-copy pointers from 0x8e0d0 are valid only during the build. Anything that
  keeps one after the build would dangle. None was observed. **inferred.**
- **Per-frame edits:** the skinning and instance writers (0xcfdb0, 0x118100, 0x11c710) allocate or
  write per-frame data. Whether any of them write into a resource-owned GPUMemory buffer was **not
  traced (unknown)**.
- **Texture streaming:** writes into the pool after build (report 11). **observed (report 11).**

## 7. Hook sites (question 4)

Signatures from `eboot_re.py sig` (unique in the text segment unless noted).

| Purpose | Site | Signature | Data at hook |
|---|---|---|---|
| Blob type tag: Mesh | entry 0x1264d0 | 32 bytes: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 78 01 00 00 48 8b 05 ?? ?? ?? ?? 48 89 b5 78 fe` | rdi[1] = stream; `[stream+0x120]` = blob, `[stream+0x118]` = size |
| Blob type tag: Texture | entry 0x98aa0 | 16 bytes: `55 48 89 e5 53 50 48 8b 47 08 48 89 f3 48 8b 77` | same stream layout |
| Blob birth | CreateResource 0x147160 | 28 bytes: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 01 00 00 4c 8b 2d ?? ?? ?? ?? 48` | decode done, blob and size known |
| Object published (range owner) | 0x14d670 | 28 bytes: `55 48 89 e5 41 57 41 56 41 55 41 54 53 50 49 89 ff 49 89 f6 bb 01 00 00 00 49 c7 c5` | wrapper and object (`[res+0x20]`) |
| GPU range: buffer | 0x8652d0 (V# setter) | 8 bytes: `c1 e1 10 c7 47 0c 00 00` | rdi = obj, rsi = base; size at `[obj+0x20]`, pointer at `[obj+0x10]` after 0x8be60 |
| GPU range: texture | 0x881e60 (T# base) | 36 bytes: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 25 ?? ?? ?? ?? 48 89 fb b9 14 06 00 00 41 89 f6 49` | base >> 8 in esi; object in rdi |
| Blob end | stream dtor 0x8dc30 | not unique in 64 bytes (2 matches) | use the CreateResource call site, or hook `[stream+0x120]` free |

Caveats:
- 0x8be60 has 106 callers (not just resources), and 0x881e60 has many. A bare hook there would be
  too broad. Gate the buffer and texture hooks on a thread-local "current factory" set at the
  0x1264d0 / 0x98aa0 entry hooks.
- The 8-byte V# signature is short. Confirm it against the image before relying on it.

## 8. Open items

1. Trace the file record's type word to the registry key (PrepareNextResourceToLoad 0x146ea0 is the
   likely place).
2. Confirm whether `PrepareForLoad`'s entry is the registry descriptor or a per-resource copy, and
   where entry+0x54 (alignment) and +0x70 (flags) are set per resource.
3. Check whether any per-frame writer touches a resource-owned GPUMemory buffer (0xcfdb0, 0x118100, 0x11c710).
4. Confirm whether streamed textures reach 0x96680 with header-only data.
5. Confirm the VirtualMemoryGPU family name at runtime (log the family name at 0x12950).
