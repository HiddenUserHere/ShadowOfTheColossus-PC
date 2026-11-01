# First serving runs (SOTC_NATIVE=1), 1 Nov

Lab route tests.json, one round each, branch re/pc-native at 0a5c9e2. Logs in `user/pcmem/<name>/logs`.

| run | switches | route | FPS | p99 ms | max ms | readbacks/s (avg) | draw work ms/frame |
|---|---|---|---|---|---|---|---|
| default-ref1 | defaults | ok | 24.8 | 118.5 | 194.0 | 433 | 41.0 |
| native-mode1 | SOTC_NATIVE=1 | ok | 27.6 | 75.0 | 182.8 | 476 | 32.5 |
| native-mode1-part | + SOTC_NATIVE_PARTICLES=prev | ok | 24.4 | 116.5 | 224.5 | 345 | 45.7 |
| native-mode1-masks | + SOTC_NATIVE_MASKS=all | crash 16 s | | | | | |
| native-serve1 | + masks + particles | crash 16 s | | | | | |

One round each: FPS, p99 and draw work vary this much between identical runs, so they are not comparable here.

## Particles (SOTC_NATIVE_PARTICLES=prev): serving works
- Served from the same-call copy 5226 times, older copy 97, exact read 11 (7 without a copy) in a window.
- The readbacks at 0x101a98 (140-150/s in the other runs) are gone from the top list.
- Visual check by the user still pending (impl-particles.md section 6).

## Masks (SOTC_NATIVE_MASKS=all): crashes
- Guest null write at 0x9427f in sub_93ef0: 0x86d210 (DCB embedded-data reserve) returned 0, then `mov dword [rbx], 0`.
  Stack 0x93dde (sub_93d70) <- 0xcd647 (sub_cd330). Reproduced twice (with and without particles); SOTC_NATIVE=1
  alone does not crash.
- Likely cause: with every mask bit set, every XPR object is drawn, and the command buffer the engine sized for the
  visible set runs out. Not proven (the DCB size and the draws per frame were not logged).
- The XPR mask consumers are the biggest readback source left (0x1a6d60: 230-280/s).
- Options: (a) enlarge the DCB the engine reserves (and measure the GPU cost of drawing every object);
  (b) last completed mask without waiting (option 3 of the plan: visibility one frame old), which needs the user's
  approval for this consumer; (c) keep the masks' GPU path.
