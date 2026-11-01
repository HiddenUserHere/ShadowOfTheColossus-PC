# Writes 7, group "heap": result

Specialist report (Haiku), saved by the coordinator from the hand-back. Read-only analysis, nothing built.

## Part 1: BpeHeap RIPs, all CPU-only allocator bookkeeping (class c), no hook

Layout: P = the user pointer (0x48c80 returns node + 0x10); P-8 = size|in-use word; P-0x10 = link word; P+0..+0x18 of
a FREE block = size-tree child links, same-size list links, parent/colour. The tree holds free blocks only.

| rip | function | written | class |
|---|---|---|---|
| 0x48b1a | Walk 0x48a70 | child link [P'+0] / [P'+8] of a free block | c |
| 0x48b3a | Walk | list link [P'+0x10] of a free block | c |
| 0x48b42 | Walk | list link [P''+0x18] of the free successor | c |
| 0x4949c | Link 0x493d0 | [P-8] of the block being freed (in-use bit cleared) | c |
| 0x49887 | Remove 0x497f0 | child slot of the free tree parent | c |
| 0x49995 | Remove | [neighbour+0x20] of a free node | c |
| 0x499e0 | Remove | [P-8] zeroed at hand-out (the caller rewrites the header) | c |
| 0x4a0ba / 0x4a1bf / 0x4a1db | rebalance 0x4a040 | colour, child, parent words of free nodes | c |

No RIP writes the user bytes of a live allocation; the owner's announce of the block covers the hand-out stores.
Small-bind "consumed" hits are page sharing: a header just below a small buffer shares its first 4 KiB page, and the
consumed test is page-granular (the tracker's upload runs are 4 KiB pages). So "consumed" means "uploaded", not "read".
(Note: the brief's 0x499e0 = 529 small / 0x49995 = 375 small came from verify 4/7, not verify 6.)

## Part 2: host writers (ucrtbase rows)

| row | routine | unannounced | consumed |
|---|---|---|---|
| +0xedeeb, +0xedece | memset 256-byte AVX loop (alignment classes of the same loop) | 1273, 1148 | 388, 284 |
| +0xee3ec | memmove 4-byte store | 361 | 360 |
| +0xee4b3 | memmove 256-byte loop | 35 | 28 |

Sources of host writes to guest memory:
1. The libc imports the port implements: `src/runtime.c` guest_memset / guest_memcpy / guest_memmove (the most likely
   dominant source). Their guest caller is known (GUEST_CALLER, recorded by SOTC_WRITE_DECL).
2. HLE result writers (savedata, services, file stat/dirent, kernel, pad, content, audio): small CPU-read results.
3. File reads (`src/runtime_file.c` do_read / do_pread copy the bounce buffer into the guest buffer): resource data
   that the GPU reads.
4. Writes through the backing view (`MemoryManager::TryWriteBacking`, buffer cache write-through, texture cache, shader
   HLE fill tables, shadow cull): they do not fault, so the oracle cannot see them; the port must announce them itself
   (inferred, not tested).

Proposals: announce after guest_memset/memcpy/memmove by caller (decide the kind from a caller histogram:
`SOTC_WRITE_DECL=2 SOTC_WRITE_DECL_MIN=1 SOTC_FPS=1`), after the file read copies (Persistent for pak reads), and in
TryWriteBacking for the GPU-read write-through sources. No announce for the HLE result writers.

Open: the top-32 truncation of the tables; the 0xee4b3 hits inside the loaded image; the memmove 4-byte callers.
