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

## Run GJ2 (rerun with hex addresses and the M fix)

Second run of the RE probe (`SOTC_RE_PROBE=1`, same probe source as run GJ). Log: `user/gj2/re_probe2.log` (5,830 `SOTC RE` lines). Parsed with two scripts kept in the session scratchpad, not in the repo.

Image base of this run is `0x17f10000` (log line 2; run GJ used `0x18040000`). The base is confirmed by the skin-queue return sites: `ret - 0x17f10000 = 0x7144f4` in 176 of 182 lines, the same site as in run GJ, so all eboot offsets below use `0x17f10000`.

Caveats:
- Nine topics reached their 300-line cap (skin-vsharp, srt-chunk, refresh, decal-cb, srt-mask, pass-mrt, skin-dispatch, skin-scratch; material reached 1,500). Every count below is over the first lines in time order, not a total. In particular no VS srt-chunk line (layout 2) appears among the kept lines, which may be a cap effect.
- The per-second rates come from `SOTC HOOKS` snapshot lines. A hook appears there only when it was called.
- `RES_CREATE` was refused in this run (detour: a branch inside the function targets its entry), so no resource dump from it. `CWKS_BODY` wrote one file: `user/re_dump/cwks_body_000000_61e4cf1dc84653b9.bin` (29,398,776 bytes, 67,045 entries).

### Hooks

Fired (17): RE_MATERIAL_FACTORY (1.4/s in the last snapshot), RE_MATERIAL_BIND, RE_SRT_CHUNK, RE_SRT_MASK, RE_REFRESH_F9C40, RE_SKIN_PALETTE, RE_SKIN_QUEUE, RE_SKIN_SCRATCH, RE_SKIN_DISPATCH, RE_SKIN_VSHARP_IN, RE_SKIN_VSHARP_OUT, RE_SKIN_VSHARP_EXTRA, RE_PASS_FLIP, RE_PASS_TABLE (0.4/s, one snapshot only), RE_PASS_MRT, RE_PASS_VIEW, RE_DECAL_ARM.

Never fired (in the manifest, absent from every rate line): RE_MATERIAL_LOADER (0xf9530), RE_REFRESH_F8D10 (0xf8d10), RE_SKIN_STORE (site 0x10c45f), RE_PASS_MODE (0xa1cd0). Same for 0xf8d10 and 0x10c45f as in run GJ. RES_CREATE was refused.

Rates over the snapshots: RE_REFRESH_F9C40 2,781 to 81,354/s (five snapshots; run GJ's 49,800/s is inside that range). RE_SRT_CHUNK 1,034 to 49,227/s. RE_MATERIAL_BIND 207 to 13,214/s.

### Manager M (pass topic)

- The global at `image+0x2ed3670` (absolute `0x1a7e3670` in this run) reads `0x0` at every logged point: pass-flip first call, pass-view (1 line each) and all 300 pass-mrt lines (`M=0x0 M60=0x0 M58=0x0`). `M2288`, `count1a8` and `M1e80` are 0 for the same reason. The fix did not produce a non-zero M in this run. Why (decompile): the only writer of `DAT_02ed3670` is `0x95a90` (`out/re/decomp/0009/00095a90.c` lines 54-61), which swaps the manager on a resolution change; that apparently never runs on this route, so the global can legitimately stay 0. `0x9fba0` does not read the global: `0x9aef0` passes its own `param_1` (`0009aef0.c` line 2183), which is `0x123c662db0` in this run. The next probe should log M as `rdi` of `0x9fba0` (or of `0x9aef0`), not the global (inference from the decompile; not yet run).
- The only manager-like pointer that is non-zero is the `rdi` of 0x9fba0 (RE_PASS_TABLE): `M=0x123c662db0`, a heap address. The probe does not read the global inside OnPassTable, so the two were never compared at the same moment.
- The probe cannot separate a stored 0 from a failed guarded read (`U64At` returns 0 on failure), and this run has no positive control for reads from image addresses.
- Verdict: NOT CONFIRMED. The hypothesis "M = global" is not supported (0 at every read), and it is not refuted either, because the global was never read at the 0x9fba0 call.

### Table

Verdict key: CONFIRMED, REFUTED, PARTIAL, STILL OPEN (not settled by this run, including "never fired" and "not hooked").

| Doc | Item | GJ2 verdict | Log evidence |
|---|---|---|---|
| texture-binding | T7.7 1 blob size per program | STILL OPEN | No program id on material lines. Blob A still varies within one mask: 0x35b has 24 distinct values (count1 > 0), 0x11b 5, 0x13b 6, 0x37b 3. |
| texture-binding | T7.7 2 block 2 vs mask bit 0x20 | CONFIRMED if "present" means gate2 > 0; REFUTED if it means entries | Bit set in 24 materials: gate2 > 0 in 24 of 24 (gate2 16 with count2 0 in 18; 32/64/96 with count2 1 in 6). Bit clear in 167: gate2 = 0 in 167 of 167. 18 of the 24 have no block-2 entries. |
| texture-binding | T7.7 3 gate byte, stages 0/1 | CONFIRMED | srt-chunk: layout 0 (222 lines) and 1 (78), `lay_u16=0x0200`, `sgpr=0`, `count=2` in 300 of 300. |
| texture-binding | T7.7 3 gate byte, VS | STILL OPEN | No layout-2 line among the 300 kept srt-chunk lines (capped). |
| texture-binding | T7.7 4 writers of P+0xb0/b8 | STILL OPEN | No write watch in this run. |
| texture-binding | T7.7 5 chunk contents | PARTIAL | Qwords logged; q0 equals P[1] (see T12.7 1). Not joined to a material. |
| texture-binding | T7.7 6 T# identity (block 1) | CONFIRMED | refresh blk 1: `A=count/count` in 300 of 300, so the T# at data+S+32i equals the payload T#. Block 2 never appears in refresh. |
| texture-binding | T7.7 7 shader s_load | STILL OPEN | DumpSrtProgram not run. |
| texture-binding | T8.9 1 0xf5cd0 exit | STILL OPEN | No RE hook. Native NATIVE_MATPARAM fires pre and post at 0xf5cd0 (about 1,815/s), but its return value is not logged by the RE probe. |
| texture-binding | T8.9 2 0xf6c90 exit | PARTIAL | 191 materials logged (src=factory); no change. |
| texture-binding | T8.9 3 pass slots 0xf8c90 / 0xf8c30 | STILL OPEN | RE_MATERIAL_BIND logs entry only. |
| texture-binding | T8.9 4 placement 0xf9c40 | CONFIRMED (0xf9c40, blk 1) | 300 of 300 with A and B full and C 0 (sample `A=7/7 B=7/7 C=0/7`). |
| texture-binding | T8.9 4 placement 0xf8d10 | STILL OPEN (never fired) | No `site=f8d10` line; absent from rate lines. |
| texture-binding | T8.9 5 call counts | PARTIAL | 0xf9c40 2,781 to 81,354/s. 0xf9ef0 not hooked. |
| texture-binding | T8.9 6 0xf9410 | STILL OPEN | No hook. |
| texture-binding | T8.9 7 0x96680 path | STILL OPEN | `path="?"` in 1,297 of 1,309 material-entry lines (run GJ: the same 1,297); 12 inline paths. |
| texture-binding | T8.9 8 0x8655d0 | STILL OPEN | Not in the RE probe. Native NATIVE_DESC_8655D0 fires (about 4,400/s, pre and post). The 0x85e8e0 writers fired (RE_SKIN_VSHARP_*). |
| texture-binding | A.7 1 blob A bytes / count | PARTIAL | count1 = list1 in 191 of 191; bytes not dumped. |
| texture-binding | A.7 2-4 pass pointer, SGPR writer, 0x86b040 | STILL OPEN | Not compared; 0x86b040 not hooked. |
| texture-binding | A.7 5 placement, S % 16 == 0 | CONFIRMED | S % 16 == 0 in 300 of 300 refresh lines; A and B identical. |
| texture-binding | A.6 placement, S % 16 != 0 | STILL OPEN | No sample: S1 and S2 are multiples of 16 in all 191 materials. |
| texture-binding | T11.6 1 S1/S2 vs 16 | CONFIRMED | 0 of 191 with S1 or S2 not a multiple of 16; no negative S. |
| texture-binding | T11.6 2-4 shader offsets | STILL OPEN | Dump / VSIZE probe not run. |
| texture-binding | T12.7 1 chunk[0] = P[0x90] (P[0]) | REFUTED | q0 equals a P[0] value in 0 of 300 lines. |
| texture-binding | T12.7 1 chunk[0] = P[0x98] (P[1]) | CONFIRMED (exact hex) | q0 equals one of 14 distinct P[1] values in 276 of 300 lines (9 zero, 15 other). Run GJ's 211 of 222 was the same relation. |
| texture-binding | T12.7 2 masks at 0x95320 | PARTIAL | 0x172 (183), 0x11b (75), 0x35b (30), 0x37b (6), 0x3db (4), 0x3fb (2). 0x37b and 0x3db are new; 0x10b, 0x12b, 0x13b absent again. |
| texture-binding | T12.7 3 walker reads | PARTIAL: VS confirmed; PS chunk[3] REFUTED | 104 programs (PS 64, VS 40), 1,407 read lines. Path [0]: VS qwords 0, 1, 2; PS qwords 0, 1. Path [12] (PS, 40 lines): qwords 0 to 3, so chunk[3] is read. Deeper paths reach qword 76 (VS). |
| texture-binding | T12.7 4-5 writers, .csdr | STILL OPEN | Not in run; RES_CREATE refused. |
| vertex-formats | T4.10 1 item store | STILL OPEN | Not hooked. |
| vertex-formats | T4.10 2 compaction | PARTIAL | Mask and P[0..9] logged (300 lines, masks above); return value not logged. |
| vertex-formats | T4.10 3 SGPR write | CONFIRMED stages 0/1; VS STILL OPEN | Same lines as T7.7 3. |
| vertex-formats | T4.10 4-5 draw side, fetch shader | STILL OPEN | Not run. |
| vertex-formats | T5.11 1-2 0x116e20 / 0x125520 | STILL OPEN | Not hooked. |
| vertex-formats | T5.11 3 per-record inputs | PARTIAL | Hook-0x11 lines from the 0x1122e0 path (return 0x11235e) in 10 lines. Input V# formats: stride 16 FLOAT 32_32_32_32 (117), stride 4 UNORM 2_10_10_10 (98), stride 12 FLOAT 32_32_32 (49), stride 80 FLOAT 32 (17), stride 4 UINT 32 (14). |
| vertex-formats | T5.11 4 output step | CONFIRMED (formats) | out_v0: stride 12, FLOAT 32_32_32 in 300 of 300. out_v1 and out_v2: stride 4, UNORM 2_10_10_10 in 300 of 300 each. |
| vertex-formats | T5.11 5 SAT V# | STILL OPEN | Not logged. |
| vertex-formats | T5.11 6 bone palette | CONFIRMED format; max BONI STILL OPEN | Palette V#: stride 16, 4,096 records, FLOAT 32_32_32_32 in 300 of 300. The base changes between dispatches (244 distinct; first 0x1040400000, then 0x10404021e0 and 0x10404043c0 in the first lines), so the palette is rewritten per dispatch. Scratch node 0 holds an identity-like 3x4 (`1 0 0 0 1 0 0 0 1 0 0 0`) in 82 lines. |
| vertex-formats | T5.11 7-8 draw side, writers | STILL OPEN | Not run. |
| vertex-formats | T14.8 1 producer data | CONFIRMED (animated, not identity) | 8 record objects. rec+0x44 takes a new value on every call (27 to 28 distinct values per record over 27 to 28 lines). rec+0x14 has 7 to 8 values per record. |
| vertex-formats | T14.8 2 queue caller | CONFIRMED | ret = eboot 0x7144f4 in 176 of 182 lines (run GJ: 172 of 178). Others: 0x493b96 (3), 0x5751dc, 0x5754dd, 0x716cab (1 each). |
| vertex-formats | T14.8 3 scratch header | CONFIRMED | dw0 = 1 and dw1 = count in 299 of 299 (count 16 to 701). |
| vertex-formats | T14.8 4 palette V#, max BONI | PARTIAL | 4,096 x 16 B confirmed; max BONI not scanned. |
| vertex-formats | T14.8 5 store to P+0x90 | STILL OPEN (never fired) | RE_SKIN_STORE absent from all rate lines; no skin-store line. |
| vertex-formats | T14.8 6-7 first draw, 0x85e5d0 | STILL OPEN | Not run; the native NATIVE_UDATA_85E5D0 hook exists but the RE probe does not log it. |
| vertex-formats | T14.8, caller of the V# writers (new) | CONFIRMED (new) | The writers are called from inside RE_SKIN_DISPATCH: ret eb:0x1939c1 (141 lines for hook 0x21) and eb:0x193a23 (94 lines for hook 0x41), both inside 0x193810 (+0x1b1 and +0x213). Run GJ's caller list was incomplete. |
| pass-targets | K6.9 1 V == M | STILL OPEN | pass-view: V = 0x101a505700, M (global) = 0x0, so "no" is not informative. |
| pass-targets | K6.9 2 Neo flag | CONFIRMED | neo = 0 (pass-flip first call and pass-table). |
| pass-targets | K6.9 3 M+0x2288, count, M+0x1e80 | STILL OPEN | Read at M = 0: M2288 = 0x0, count1a8 = 0, M1e80 = 0x0. |
| pass-targets | K6.9 4 derived tables | CONFIRMED for the object read | pass-table at M = 0x123c662db0 (one line): all ten pairs M+0xb40+idx and M+0x1680+idx are 0x0, and the values never changed across calls. |
| pass-targets | K6.9 5 R, S, mode | PARTIAL | pass-flip: R = 0x123c3c5630 in all 4 logged calls; S+0x4c0 = 3 and S+0x4b8 = 0 in all. pass-mode (0xa1cd0) never fired, so "no mode change" is not established. |
| pass-targets | K6.9 6 decal CB INFO | REFUTED as stated (unchanged) | 40 decal arms, 260 draws logged. Bound bit 0 set in 180 (69%); cb0 raw word non-zero in 208 (80%). 80 draws have bound 0x00, 52 of them with a non-zero cb0 raw word (0x00050410 in 50). Pixel hash UNKNOWN. |
| pass-targets | K6.9 7 0x92140 slots | PARTIAL: P[1] not a pointer CONFIRMED; meaning STILL OPEN | slot1 is a pointer in 0 of 300 lines and a 0xffff... pattern in 300 of 300. slot0 = P + 0x280 in 284 of 300. |

### Addresses in hex (cross-match)

- Host-side objects sit in the 0x12... heap: materials 0x123c6b80f0 to 0x1244ddc890 (191 distinct), P objects 0x124b8e0000 to 0x1253acfb80 (192 distinct), payloads 0x124670fc00 to 0x124ad2fdc0, refresh data 0x124b360480 to 0x124b38a000, palette objects 0x123c688c18 and up.
- GPU-side addresses are in 0x10... to 0x12...: srt chunk pointers 0x1040300080 to 0x104127c710; P[1] values 0x1040300000 to 0x1042710110; T# bases (word0 << 8) 0x102bf00000 to 0x12a5c00000 (230 with top byte 0x10, 1,079 with 0x12); palette V# bases 0x1040400000 to 0x1119c00640; out V# bases 0x106adb0c10 to 0x111dcdc210; skin-vsharp V# bases 0x1040400000 to 0x12539d1be0.
- Exact-hex matches across event kinds: chunk q0 = P[1] (276 of 300); refresh A = payload T# (300 of 300); palette V# base starts at 0x1040400000, the same as the first skin-vsharp base.
- Open point: 1,079 T# bases decode into 0x12..., the same range as the host heap objects (for example the T# `12557000 26300000` in payload 0x1249f1ea80 gives base 0x1255700000). It is not resolved whether these are guest texture addresses (the GPU V# bases use the same window) or a misread of a host pointer.
- Other observation: pass-view `F40 = 0x123c688c18` equals the first skin-palette record (`rec=0x123c688c18`). Possibly a reused allocation; not analysed.

### Summary of changes from run GJ

- Confirmed with hex: T7.7 3 (stages 0/1), T7.7 6, T8.9 4 (0xf9c40), A.7 5, T11.6 1, T12.7 1 (P[1], not P[0]), T14.8 2 (176 of 182), T14.8 3, T5.11 4, K6.9 2, K6.9 4.
- Refuted: T12.7 1 for P[0x90]; T12.7 3 for PS chunk[3] (read via path [12]); K6.9 6 as stated (still).
- Still open, and why: the M global (reads 0 everywhere; the 0x9fba0 call was not compared), 0xf8d10 / 0x10c45f / 0xa1cd0 / 0xf9530 never fired, VS srt-chunk missing (capped), and most other items were not hooked or not run.

## Resource dump check (run GJ3)

Input: `user/re_dump/` from the GJ3 run (20 `cmat_*`, 20 `cprt_*`, 1 `cwks_body_*`; name map in `user/gj3/re_probe2.log`,
`SOTC RES DUMP` lines). The dumper (`gpu/sotc/probe_dump.cpp`) writes the buffer the loader hands to the memory stream
(`cmat`, `cprt`: the resource body as read, no name and no added header), and for `cwks` the body from the count dword
(the `PSKW` header is not written). So every check below is on file bytes. The in-memory object (`+0x450`, `+0x5b0`,
`+0x560`, `+0x468`, `P+0xb0`, ...) is not in the dump; a claim about it is checked only through its file inputs.
Parsers (read-only, outside the repo): `C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/`
(`cmat_full.py`, `extra_checks.py`, `cprt_check.py`, `cprt_more.py`, `cprt_stats.py`, `cwks_check.py`, `cwks_deps2.py`).
Offsets are file offsets. The `.cmat` header dwords are numbered 1..13 as in `materials-meshes.md`.

Verdicts: CONFIRMED, REFUTED, or CANNOT CHECK (not decidable from this dump).

### .cmat (20 files, sizes 155 to 2,179 bytes)

| Claim (doc) | Verdict | Evidence |
|---|---|---|
| File body: dwords 1..6 (A..F), count N (dword 7), N slot records, list-1 count and refs, list-2 count and refs, size A (dword 10), size B (dword 11), blob A, blob B, map-1 size and bytes, map-2 size and bytes | CONFIRMED | Parse ends exactly at EOF in 20 of 20 files. Slot record: u32 index, u32 mask, u8 length, path. List ref: u8 length, path. Order note: each list count is followed by its refs, and the two blob sizes come after both lists. |
| Header is the `STAM`/version-57 wrapper of `0xf9530` | CANNOT CHECK | The dump starts at dword A; no file starts with `STAM`. The wrapper is stripped before the stream or is not on this path. |
| Slot index k = pass index; slot at `material+0x60+0x48*k` | CONFIRMED for the index and the names | Indices seen: 0, 1, 2, 3, 4, 5, 7, 8, 10, 12, 13 (max 13). Basenames agree with the doc labels: 0 shadow, 1 depth, 2/3 gbufferbakedambient (+motion), 4/5 gbufferdynamicambient (+motion), 7 standard, 8 alpha, 10 depthfadeout, 12 depthfadeoutinvlodfade, 13 shadowfadeout. Not seen: 6 (decal), 11 (debug). |
| Slot mask (second u32 of the slot entry, `B+0x38`) is in {17, 23, 25, 31} | CONFIRMED for the position; only 17 seen | All 110 slot masks are 0x11 (17). 23, 25, 31 do not occur in these files. |
| rec[0] = VS and rec[4] = PS (mask 17 = bits 0 and 4) | CANNOT CHECK | A slot holds one `.csdr` path, not two record paths. The stage codes are inside the `.csdr`, which is not dumped. |
| The slot names a `.csdr` (its OrbShdr footer hash = predict table) | PARTIAL | All 110 slot paths end in `.csdr`. The footer hash needs the `.csdr` bytes. |
| `materials-meshes.md` 3.1: the 14-slot entries are texture slots (`texRes` is a texture, `view[]` T#/V# pointers) | REFUTED as stated | The 14-slot entries hold shader (`.csdr`) paths and masks. Texture references are in the two lists (next row). |
| `materials-meshes.md` 3.4: list 1 and list 2 are "most likely shader collections" (inferred) | REFUTED | List 1: 81 refs, all `.ctxr` (textures), in 10 files. List 2: 4 refs in total, all 4 empty (length 0), in the four font files only. |
| Blob sizes are 16-byte multiples (run results T11.6 1 and A.7 5; `S % 16 == 0`) | CONFIRMED on file | Size A values 0, 16, 32, 48, 128, 176, 192, 208, 256; all 20 files are 0 mod 16. Size B values 0, 16, 32; all 20 are 0 mod 16. |
| Map records are 8 bytes (`+0x4f0`, `+0x528`) | CONFIRMED on file | Map-1 sizes 0 or 40; map-2 sizes 0 or 8. All multiples of 8. |
| Gate of block 1 (`+0x5b0`) = blob A bytes + 32 x list-1 count; gate of block 2 (`+0x5f0`) = blob B bytes + 32 x list-2 count | CANNOT CHECK the field; inputs consistent | Runtime fields. The file inputs give a non-zero block 1 in 18 of 20 files and block 2 in 6 of 20 (font files 6, 8, 9, 10, and files 11, 19). |
| Header dword B (`+0x458`, "kind") selects the state object; D (`+0x460`) is the pass kind | CANNOT CHECK the meaning; values noted | B is 0 (10 files) or 4 (10 files). D is 0 (17 files) or 1 (3 files). The kind values 6, 3, 8 that the selector tests do not occur. |
| Dword C (`+0x464`, flags, bit 0x40 tested) | CANNOT CHECK (bit 0x40 never set here) | C is 0x4000000e (10 files), 0x4000002e (9), 0x40000022 (1). Bit 0x40 is 0 in all 20, so the `flagsC & 0x40` branch is not exercised by this sample. |
| Dword E is constant | CONFIRMED (value) | 0xffffffff in all 20. Meaning not in the doc. |
| Dword F (`+0x45c` uses 2 - F) | CONFIRMED (value set) | F is 0 (15 files) or 2 (5 files). |
| `+0x450` populated mask, `+0x560` state selector, `+0x468` render flags | CANNOT CHECK | Runtime fields. |

### .cprt (20 files, 31 emitter records in total)

| Claim (doc) | Verdict | Evidence |
|---|---|---|
| Header +0x04 = N, the record count | CONFIRMED | Each file has N records (1 to 4). Every record resolves to a name and blobs inside the file. |
| Record i at `(0x74+4i) + rel_i`, rel relative to the field address | CONFIRMED | All 31 records and all name and blob refs resolve with this rule. |
| Key table: count at +0x0c, rel at +0x6c, sorted | CONFIRMED (sorted) | Keys are in non-decreasing order in all 20 files. Counts are 0 to 3; the table is empty in 14 files. |
| R+0x04 = blob size S, blob at R+0x14+rel, copied verbatim | CONFIRMED (in range) | Blob ends are inside the file in 31 of 31 records. Sizes 172 to 560 seen. Only 2 of 31 are 16-byte multiples (not claimed). |
| R+0x08 stride, R+0x0c and R+0x10 dispatch values, R+0x1c spawn multiplier | CANNOT CHECK meaning; values noted | R+0x08 = 8 in all 31. R+0x0c = 16 in all 31. R+0x10 = 4 in all 31. R+0x1c is 100 in 19 of 31 records; the other 12 vary (values up to 10000). |
| R+0x20 parent index, -1 = none | CONFIRMED (value) | -1 in all 31 records. No child emitters in the dump, so the child pass is not exercised. |
| R+0x24 = emitter name (C string) | CONFIRMED | 31 of 31 are printable NUL-terminated strings. |
| R+0x28, +0x2c, +0x30 = three `.csdr` names; labels in particles-engine 2d and 3c: ref 1 "enqueue/spawn", ref 2 "process spawn", ref 3 "update" | CONFIRMED (three names); REFUTED (role order as labelled) | All 93 refs are `.csdr`. Name suffixes by field: +0x28 emit, +0x2c spawn, +0x30 update, in all 31 records. The field order is emit, spawn, update; the doc labels for the first two fields do not match it. This also answers the open question of particles-data 3c (the names are resolved; the roles are the suffixes). |
| Every emitter has exactly three named shaders | CONFIRMED | 31 of 31 have three non-empty names. |
| +0x34 "typed block A" (`0xff430`) | CANNOT CHECK meaning | Non-null in 31 of 31, and it always points to the same 2 bytes (0x40, 0x00). Not a name. |
| +0x38 "typed block B" (mesh emission; mesh path when non-null) | CONFIRMED (no mesh emitter here) | Null in all 31 records. |
| +0x3c "typed block C" | CONFIRMED (null) | Null in all 31. |
| +0x40 "typed block D" holds a resource ref (checked by `0xa8700`) | PARTIAL | Non-null in 2 records, both in one file, and they hold texture (`.ctxr`) refs. |
| Header +0x58 type index, +0x5c and +0x60 floats | CANNOT CHECK meaning; values noted | +0x58 is 0 in 19 files and 1 in one file. The floats are (500, -0.01) in 17 files and (0, 0) in 3. |

Still open after this dump: the meaning of typed blocks A and C, the R+0x08/0x0c/0x10 semantics, and the header bytes the
doc lists as not read. The `.csdr` contents (footers, record stage codes) need a `.csdr` dump.

### cwks body (1 file, 29,398,776 bytes, 67,045 entries)

| Claim (doc) | Verdict | Evidence |
|---|---|---|
| File header `PSKW`, version 6, then the body | CANNOT CHECK | The dump starts at the count dword (67,045). No `PSKW` in the dump. |
| Body: count N, then N entries of 16 bytes (key, name_off, dep_count, deps_off) | CONFIRMED | Each entry's name and list offsets are inside the body. |
| dep_end = table + 4 + 16N (name and list offsets are relative to it) | CONFIRMED | Base 1,072,724 (4 + 16N). All 67,045 names are printable NUL-terminated strings at base + name_off. |
| Key = FNV-1a 32 of the entry name | CONFIRMED | Equal for 67,045 of 67,045 names. Lowercase also matches every name, so case is not decided by this check. |
| Entries sorted by key (binary search, `0x14e560`) | CONFIRMED with 2 collisions | Non-decreasing. Two keys occur twice, each with two different names (0x59d9d31b and 0xa197db1e). A search on key alone cannot separate such a pair. |
| Dependency list item: u8 len, name, NUL; next item at +len+1 (open detail in the doc) | CONFIRMED | len counts the NUL. 340,920 items parse with the NUL at len-1 and no failure. The "bytes after the name must not be NUL" question is moot: the NUL is inside len. |
| Dump extent = largest end of the names | CONFIRMED | Largest name end equals the dump size minus base. Dependency lists end before it. |
| Entries are resources, not areas; dependency lists hold `.cmat` and `.cslt` (inferred) | CONFIRMED for `.cmat`; `.cslt` absent | Entry kinds: cmdl 24,540, csdr 17,196, cmsh 13,545, cmat 10,365, cprt 744, cgpr 335, csml 189, cvmg 58, cvgs 45, cfon 26, calt 2. Dependency items: ctxr 85,326, csdr 84,354, cmsh 62,286, cmat 61,702, cmdl 19,559, cclm 14,661, cpid 9,187, cptx 1,560, cprt 699, cvmg 287, csli 233, cskl 197, csml 189, cgpr 173, cslt 0. |
| Dependency kinds per entry kind (not in the doc) | observed | cmdl entries depend on cmat, cmsh, cclm, cpid. cmat entries depend on ctxr and csdr. cprt entries on csdr, cptx, cmsh, cmdl, ctxr. csdr entries have no dependencies (all 17,196 csdr entries are zero-dependency entries). |

### Surprises

1. The `.cmat` list 1 holds texture paths, not shader collections; the 14-slot array holds shader paths. The
   `materials-meshes.md` 3.1 and 3.4 readings are both wrong on this point.
2. The `.cprt` name suffixes are emit, spawn, update at +0x28, +0x2c, +0x30. This resolves the emitter-name question
   of particles-data 3c for these 20 files.
3. The `.cwks` sorted table has two key collisions, and its dependency lists contain no `.cslt` entries.
4. A `.cmat` slot is one `.csdr` path, so the two-record (rec[0]/rec[4]) layout cannot be checked from a `.cmat` alone.
