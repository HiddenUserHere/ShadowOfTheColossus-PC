# Run results summary (RA, 4 Nov 2026)

One run of the observe-only RE probe (`SOTC_RE_PROBE=1`, cold shader cache). Log: `user/runGJ/re_probe.log`. Probe:
`gpu/sotc/probe_re.cpp`; walker log: `LogWalkerRe`. Eboot offset = absolute address - 0x18040000 (image base of this run).
Every topic is capped at 300 distinct keys (material at 1500 lines), so counts are the first lines in time order.
Full sections with evidence: `texture-binding.md`, `vertex-formats.md` and `pass-targets.md` ("Run results (RA, 4 Nov 2026)").

Important probe defect: the manager M is read as `*(*(image+0x2ed3670)+0x80)`, which is 0 in this run. The manager is
`*(image+0x2ed3670)` itself (decompile and the 0x9fba0 argument). Every M-based value of this run is invalid.

| Doc | Item | Verdict | Key evidence |
|---|---|---|---|
| texture-binding | T7.7 1 blob size per program | UNKNOWN | no program per material; size varies within a mask (0x35b: 24 values) |
| texture-binding | T7.7 2 mask per program | PARTIAL | block 2 present iff mask bit 0x20 (24 of 24; 0 exceptions in 191) |
| texture-binding | T7.7 3 gate byte, stages 0/1 | CONFIRMED | `layout=0 lay_u16=0x0200 sgpr=0 count=2` (222), index 1 (78) |
| texture-binding | T7.7 3 gate byte, VS | UNKNOWN | index 2 not among the first 300 keys |
| texture-binding | T7.7 4 writers of P+0xb0/b8 | UNKNOWN | no write watch |
| texture-binding | T7.7 5 chunk contents | PARTIAL | 8 qwords logged; no join with material |
| texture-binding | T7.7 6 T# identity | CONFIRMED (block 1) | refresh rows: payload T# = block T# |
| texture-binding | T7.7 7 shader s_load | UNKNOWN | DumpSrtProgram not run |
| texture-binding | T8.9 1 0xf5cd0 exit | UNKNOWN | no hook |
| texture-binding | T8.9 2 0xf6c90 exit | PARTIAL | material lines, 191 materials |
| texture-binding | T8.9 3 0xf8c90/0xf8c30 pass slots | UNKNOWN | only entry logged |
| texture-binding | T8.9 4 0xf8d10 / 0xf9c40 placement | CONFIRMED (0xf9c40, block 1); UNKNOWN (0xf8d10) | 300 of 300, `A=10/10 B=10/10 C=0/10`; f8d10 never fired |
| texture-binding | T8.9 5 call counts | PARTIAL | 0xf9c40 about 49,800/s; 0xf9ef0 not hooked |
| texture-binding | T8.9 6 0xf9410 | UNKNOWN | no hook |
| texture-binding | T8.9 7 0x96680 path | UNKNOWN | path read failed (`path="?"` 1297 of 1309) |
| texture-binding | T8.9 8 0x8655d0 | UNKNOWN | not hooked (V# writers at 0x85e8e0 etc. are) |
| texture-binding | A.7 1 blob A bytes/count | PARTIAL | count = count1 in 191 of 191; bytes not dumped |
| texture-binding | A.7 2-4 pass pointer, SGPR writer, 0x86b040 | UNKNOWN | not compared / not hooked |
| texture-binding | A.7 5 placement | CONFIRMED for S % 16 == 0 | A.6 (S % 16 != 0) not settled: 0 such materials |
| texture-binding | T11.6 1 S1/S2 vs 16 | CONFIRMED (this run) | 0 of 191 with S % 16 != 0 (S2 too) |
| texture-binding | T11.6 2-4 shader offsets | UNKNOWN | dump / VSIZE probe not run |
| texture-binding | T12.7 1 chunk[0] = P[0x90] | REFUTED (sampled rows) | chunk[0] = 0x1042710110 = P[1] (P+0x98) in 211 of 222; P[0] = 0 for mask 0x172 |
| texture-binding | T12.7 2 masks at 0x95320 | PARTIAL | 0x172 (189), 0x11b (75), 0x35b (32), 0x3fb (4); 0x10b/0x12b/0x13b absent |
| texture-binding | T12.7 3 walker reads | CONFIRMED | VS: chunk[0], [1], [2]; PS: chunk[0], [1]; no read of chunk[3]/[4] in 28 programs |
| texture-binding | T12.7 4-5 writers, .csdr | UNKNOWN | not in run |
| vertex-formats | T4.10 1 item store | UNKNOWN | not hooked |
| vertex-formats | T4.10 2 compaction | PARTIAL | mask and P[0..9] logged; return value not |
| vertex-formats | T4.10 3 SGPR write | CONFIRMED (stages 0/1) | count 2, SGPR 0; VS UNKNOWN |
| vertex-formats | T4.10 4-5 draw side, fetch shader | UNKNOWN | not run |
| vertex-formats | T5.11 1-2 0x116e20 / 0x125520 | UNKNOWN | not hooked |
| vertex-formats | T5.11 3 per-record inputs | PARTIAL | input V# formats logged via 0x1122e0 (idx 17-21) |
| vertex-formats | T5.11 4 output step | CONFIRMED (formats) | out_v0 FLOAT 32_32_32 stride 12; out_v1/v2 UNORM 2_10_10_10 stride 4 |
| vertex-formats | T5.11 5 SAT V# | UNKNOWN | not logged |
| vertex-formats | T5.11 6 bone palette | PARTIAL (plausible) | scratch node 0 holds plausible 3x4 floats |
| vertex-formats | T5.11 7-8 draw side, writers | UNKNOWN | not run |
| vertex-formats | T14.8 1 producer data | CONFIRMED animated, not identity | 216 calls; rec+0x44 changes each cycle (27 values per record) |
| vertex-formats | T14.8 2 queue caller | CONFIRMED | 172 of 178 from eboot 0x7144f4 (sub_714410, site 0x7144ef); not in T14.3 |
| vertex-formats | T14.8 3 scratch header | CONFIRMED (header), PARTIAL (permutation) | dw0 = 1, dw1 = count, 297 of 297 |
| vertex-formats | T14.8 4 palette V#, max BONI | PARTIAL / UNKNOWN | palette V# 4096 x 16 B logged; max BONI not scanned; pal_base defect |
| vertex-formats | T14.8 5 store to P+0x90 | UNKNOWN | RE_SKIN_STORE never fired |
| vertex-formats | T14.8 6-7 first draw, 0x85e5d0 | UNKNOWN | not run / not hooked |
| pass-targets | K6.9 1 V == M | REFUTED | V = 0x100dc05700 is not the manager O = 0x78322817712 |
| pass-targets | K6.9 2 Neo flag | CONFIRMED | neo = 0 |
| pass-targets | K6.9 3 M+0x2288, count, M+0x1e80 | UNKNOWN | read at the wrong base |
| pass-targets | K6.9 4 derived tables | CONFIRMED (empty on base PS4) | 10 sources 0x0/0x0 at the correct object |
| pass-targets | K6.9 5 R, S, mode | PARTIAL | R = 0x123c3c5630 in 4 calls; no mode change (inferred) |
| pass-targets | K6.9 6 decal CB INFO | REFUTED as stated | cb0 enabled in 184 of 262 draws; pixel hash UNKNOWN |
| pass-targets | K6.9 7 0x92140 slots | UNKNOWN | wrong base; P[1] never a pointer |
