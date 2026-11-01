# Native gaps, round 2 (1 Nov): results

Specialists (Haiku, worktrees) on the gaps of impl-writes7.md, integrated and built by the coordinator. Verify
sessions 9-11 (`user/pcmem/native-verify9..11`), host-write histogram `user/pcmem/native-hostwrites`.

## Fixed
| gap | change | commit |
|---|---|---|
| Final DCB of each frame never announced | SubmitJob mode 1 (0xcefa0) submits through 0x91f90 -> Gnmx_InternalSubmitAndFlip, past the 0x91f80 hook. NATIVE_DCB_SUBMIT_FLIP (site 0x91f97) runs the DcbSubmit body there; verify 10: 30 calls/s, once per frame | c51f128 |
| ACE ring dwords never announced | the IB packets written by 0x884c40 are read in place after the doorbell; announced in sceGnmDingDong ("ring") | c51f128 |
| CComputeContext | its only command memory is the CCB, already announced by CcbSubmit; its second block is never used | (finding) |
| Guest copies of GPU data | guest memcpy/memset/memmove from 0xf6eac9 (V# descriptor clears), 0xf9d7d (material params), 0xfce45 (render-thread slot state) announced PerFrame ("gmem", ~800/s). 0x8bf50 (resource creator) is already NATIVE_RES_COPY; 0x918e7 (CRenderContext ctor memset) is a CPU object. File reads are not announced (CPU-only buffers would be uploaded) | 1b51f99 |
| 0x18c3be binding record | NATIVE_RECORD_18C390 announces +0x278..+0x298 PerFrame ("rec"); a V# over it is not proven | 6a37737 |
| Label-wait counter had no caller | XPR spins (when not removed) and the ring8 flip's WaitGpuIdle report; the flip's tick wait is the reuse fence (frame pacing) and is not counted. Verify 10: label waits 0 | 6a37737, 1970247 |
| Windows could not register | blocks are imported at commit, not at boot; only committed blocks are pinned | 1b51f99 |

Verify 10: unannounced writes ~47.6k per session (55-62k before this round); route done, no crash.

## Windows: registered, but never serve (verify 11, new stage counters fc2d44d)
- A gpu-arena: 2 blocks routed in the session, both refused by the fresh writer's batch gate (the guest's
  vmem batch queue is not empty). Kind-2 arena blocks are rare (4 per session, the first one before the window
  setup); the per-frame heap reuses live chunks. Ring8 (the default) already imports these arena blocks.
- B mask-heap: 6 key misses, 0 routed: the 8 MiB kind-1 blocks do not reach the window with memtype key 17. The
  memtype 17 blocks are announced instead (98adc9a).
- C render-targets: the RT block is a would-crash (bitmap has no free bit, free-VA list empty).
So each window has its own blocker in the pool's fresh path, and the data each would hold is already covered (ring8
imports, memtype 17 announces, render targets are GPU-written). Decision for the user: fix the three pool gates, or
drop the windows in N4.
