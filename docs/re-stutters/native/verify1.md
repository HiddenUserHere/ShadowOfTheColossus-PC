# First native verify session (1 Nov)

Run: `tools/pcmem_matrix.py --configs default --repeat 1 --env SOTC_NATIVE=2 --env SOTC_VERIFY=1
--env SOTC_NATIVE_PARTICLES=prev`, Lab route `user/routes/tests.json`, log
`user/pcmem/native-verify1/logs/default-r1-a1.log`. Branch re/pc-native at 95b5251. The route finished: 208 frames,
29.0 FPS, p50 33.3 ms, p99 58.5 ms, max 137.3 ms. No crash.

## Particles (SOTC_NATIVE_PARTICLES=prev, verify): pass

| window | calls/s | counts compared | mismatched | same-call (must be 0) | copy 0 for a live count | exact reads (no copy) |
|---|---|---|---|---|---|---|
| boot | 44 | 2398 | 0 | 0 | 0 | 8 (0) |
| | 120 | 4800 | 0 | 0 | 0 | 0 (0) |
| | 565 | 22265 | 0 | 0 | 0 | 174 (0) |
| route | 2145 | 99597 | 3 | 0 | 0 | 22 (0) |
| | 2097 | 103466 | 20 | 0 | 0 | 34 (0) |
| | 1835 | 87718 | 17 | 0 | 1 | 14 (0) |
| exit | 1445 | 66847 | 1 | 0 | 0 | 11 (7) |

- Every array was copied (GPU copies ~ registered). The copy of the same call never differed from the exact read.
- The few mismatches come from the older copies (c - 3, c - 4), as expected. One of them was a 0 copy for a live count:
  a system that started emitting. A young system is not retired (age test > 1 s), so that is harmless. An
  AutoDestroy system older than 1 s that starts emitting again would be the risk, which is why the visual check
  is still needed.
- `live 512` is the record cap (kMaxRecords). At ~2100 calls/s (~70 pools a frame) it holds ~7 frames, more than
  the 4 calls serving needs.
- Next: a serving run (`SOTC_NATIVE=1 SOTC_NATIVE_PARTICLES=prev`) and the user's visual check (impl-particles.md
  section 6).

## N2 gate (unannounced writes must be 0): not met

`SOTC NATIVE verify: unannounced writes` 4386 / 5343 / 6549 / 11315 / 11754 / 16654 / 13464 per 5 s window (~2.7k/s
on the route). gpu-written reads 0 and label waits 0, but readbacks still run at ~1000/s in this mode, so the
gpu-written read counter is probably not fed by the readback path. Check it before trusting the 0.

Top RIPs (per-window tables summed; the tables list only the top RIPs, 40131 of 69465 writes):

| rip | writes | function | share (cumulative) |
|---|---|---|---|
| 0x101ae4 | 5827 | ParticleSystemPool::ProcessStats (pool tables, CPU only) | 14.5% |
| 0x865eaa | 4416 | sub_865ea0 | 25.5% |
| 0x16e9dc | 3189 | sub_16e990 | 33.5% |
| 0x17c4f | 2145 | sub_179c0 | 38.8% |
| 0x48b42 / 0x48b1a / 0x48b3a | 3614 | BpeHeap_Walk (heap block headers) | |
| 0x86f308 | 1417 | sub_86f2a0 | |
| 0x4949c | 1237 | BpeHeap_Link | |
| 0x1d849 | 1189 | sub_1d750 | |
| 0xc7acc | 977 | sub_c7850 | |
| 0x17f25 | 859 | sub_17e90 | |
| 0x917f2 | 836 | sub_917c0 | |
| 0xebe220 | 816 | sub_ebe200 | |
| 0x49887 | 740 | BpeHeap_Alloc | |
| 0xbbbe4 | 657 | sub_bbbe0 | |
| 0x861522 | 591 | sub_861520 | |
| 0xa26eb0, 0x50a1ce, 0xf3b026, 0x181258, 0xceaa9, 0x184856, 0xa3fb73, 0x1d4fbe, 0x11bd99 | 393-580 each | | 85% at 0x11bd99 |

64 RIPs in all; 16 of the first sample lines come from the port itself (host RIP 0x7ff7b7585ee2).

Reading: most of the top RIPs are CPU bookkeeping (the heap allocator's block headers, the particle pool's tables),
not data the GPU reads. They count only because they share a tracked page with GPU-read data. The counter does
not tell those apart from real misses, so before patching every site it should count only the unannounced
writes whose bytes the GPU binds later (an overlap check at bind time in verify mode). A miss the GPU reads
then needs an announce; a write the GPU never reads needs nothing once page tracking is deleted.
