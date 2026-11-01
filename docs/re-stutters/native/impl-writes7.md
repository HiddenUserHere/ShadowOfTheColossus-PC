# Writes wave 7: result (1 Nov)

Brief: WRITES7-BRIEF.md. Data: verify6-consumed.md, verify sessions 7 and 8 (`user/pcmem/native-verify7`, `-verify8`).
Group reports: impl-writes7-{particles,desc,core,render,region,heap}.md.

## What the verify oracle measures
- An unannounced write is the first write to a tracked 4 KiB page after its protection (one write per page per cycle),
  so the RIP names the page's first writer, not necessarily the writer of the GPU data on it.
- "Consumed" means the old path uploaded the page within 4 frames. The upload is page-granular and the bind range is
  clamped to the 64 KiB granule (ClampRangeSize), so consumed = uploaded, not read. Almost all consumed takes came
  through read-only binds (verify 7), so written binds do not explain them either.
- CPUMemory granules mix GPU-read blocks and CPU objects, so the gate "unannounced writes = 0" cannot be reached
  while page tracking is the oracle.

## Classification (about 40 RIPs)
| group | RIPs | result |
|---|---|---|
| particles | 0x101ae4, 0x119e27, 0x11bd99 | CPU bookkeeping (life-stamp table; no V# over it found) |
| desc | 0x865eaa, 0x86f308, 0x861522, 0x16e9dc | CPU object headers and flags; the command bytes are announced at submit |
| core | 0x17c4f, 0x17f25, 0x1805f, 0x1806a, 0x184856, 0x917f2 | bucket allocator bookkeeping, context objects |
| render | 0xd4d97, 0xd3b7f, 0xf3b026, 0xa3fb73, 0x18c3be | CPU containers; 0x18c3be open (a 0x298-byte binding record) |
| region | 0x1d849, 0xceaa9, 0xc7acc, 0x1a8ca1, 0xc14c6, 0xbbbe4 | RenderSingleFrameCPU (memtype 17) blocks are GPU data: **fixed** (98adc9a); the rest CPU tags |
| heap | BpeHeap_* (10 RIPs), ucrtbase memset/memmove | allocator headers; the host writes come from guest_memset/memcpy/memmove imports, file reads, HLE |

Fixed: memtype 17 blocks announced PerFrame at submit (verify 8: 0xc7acc, 0xc14c6, 0x1d849 left the lists).
Also fixed this wave: garbage instance announces (faf7711), the CP's own fence writes counted as engine writes
(589c2db), the gpu-written read counter wired to the read faults.

## Found gaps (not fixed)
1. Native windows never register: a reserved window has no backing until the game commits it, so the Vulkan host
   import fails ("import failed", verify 7). Importing all three would pin ~496 MiB. Needs per-commit import.
2. Host writes of GPU data: guest_memset/memcpy/memmove (src/runtime.c), file reads into resource buffers
   (src/runtime_file.c), backing-view write-throughs (TryWriteBacking): the port must announce them itself; the
   callers need a histogram first (SOTC_WRITE_DECL=2 SOTC_WRITE_DECL_MIN=1).
3. CComputeContext DCB/CCB may not be in KickoffSubmitJob's lists (desc report, open 1).
4. 0x18c3be binding record consumer.
5. The label-wait counter (VerifyGpuWait) still has no caller.

## The gate from here
The exact test is the one N4 performs: tracking off for announced data, rendering checked against the tracked run.
