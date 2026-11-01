# Verify session 6: unannounced writes the GPU took (1 Nov)

Log `user/pcmem/native-verify6/logs/default-r1-a1.log`. Branch re/pc-native, commit faf7711. Run: SOTC_NATIVE=2 SOTC_VERIFY=1
SOTC_NATIVE_PARTICLES=prev, Lab route tests.json, one round. Route done: 204 frames, p99 67.1 ms.

Totals over the session: 57458 unannounced writes, 44174 of them consumed (the old path uploaded the written bytes
within 4 frames). Of the consumed ones listed (the per-window top 32), 18035 of 32448 went through a bind of at most
64 KiB: a read the shader almost surely makes. The rest came through large binds (up to 64 MiB: a V# over a whole
heap), which may only cover the bytes.

Columns are sums of the per-window top-32 tables, so a RIP that falls out of a window's top list is undercounted.
`host:bb-probe.exe+0x3f8eb2` is the command processor's RELEASE_MEM fence (a GPU write, excluded from the oracle
from the next build on) and `host:bb-probe.exe+0x241e97` is sceGnmDingDong (the port's own write). The
`host:ucrtbase.dll` rows are memcpy/memset calls from host code (the caller is unknown: loader file reads or HLE).

| rip | unannounced | consumed | via binds <= 64 KiB | largest bind | image | last address |
|---|---|---|---|---|---|---|
| 0x101ae4 | 4792 | 4706 | 4706 | 65536 | 0 | 0x140b607530 |
| host:bb-probe.exe+0x3f8eb2 | 3623 | 2596 | 2009 | 196608 | 0 | 0x1253809228 |
| 0x865eaa | 3454 | 1614 | 1104 | 196608 | 0 | 0x1253806b20 |
| 0x16e9dc | 2455 | 1335 | 961 | 196608 | 0 | 0x12505d8868 |
| 0x1d849 | 1247 | 1235 | 0 | 983040 | 0 | 0x13ce97f2d8 |
| 0x17c4f | 1845 | 1167 | 745 | 67108864 | 0 | 0x1043000008 |
| 0x86f308 | 1031 | 1025 | 487 | 196608 | 0 | 0x1253b35710 |
| 0x18c3be | 996 | 996 | 3 | 196608 | 0 | 0x13d5cc7870 |
| 0x48b1a | 1342 | 994 | 59 | 52887984 | 0 | 0x11445fe048 |
| 0x4949c | 1379 | 931 | 45 | 48201280 | 0 | 0x10acded148 |
| 0x48b3a | 1130 | 756 | 199 | 52887984 | 0 | 0x10b16f9360 |
| 0x48b42 | 1394 | 723 | 41 | 67108864 | 0 | 0x10de8f67a8 |
| 0x17f25 | 772 | 657 | 175 | 28595840 | 0 | 0x10a460ffd8 |
| 0x917f2 | 886 | 502 | 379 | 131072 | 0 | 0x124bd70008 |
| 0x119e27 | 375 | 501 | 463 | 131072 | 0 | 0x13a1ae7ec0 |
| host:bb-probe.exe+0x241e97 | 374 | 495 | 495 | 65536 | 0 | 0x124562ef28 |
| 0xd4d97 | 556 | 494 | 494 | 65536 | 0 | 0x1362e04f40 |
| 0x49887 | 613 | 444 | 41 | 52887984 | 0 | 0x1358995210 |
| 0x861522 | 304 | 443 | 348 | 131072 | 0 | 0x124d54593a |
| 0x1805f | 478 | 419 | 90 | 28596992 | 0 | 0x10d6c0c3c8 |
| 0xceaa9 | 287 | 414 | 0 | 983040 | 0 | 0x13ce9179e4 |
| 0x1806a | 464 | 400 | 85 | 24454896 | 0 | 0x10d6c051c0 |
| host:ucrtbase.dll+0xedeeb | 1273 | 388 | 372 | 131072 | 0 | 0x1252b50000 |
| 0xc7acc | 386 | 375 | 170 | 917504 | 0 | 0x13ce910000 |
| 0x184856 | 376 | 375 | 375 | 65536 | 0 | 0x123c7bae18 |
| 0x1a8ca1 | 372 | 372 | 0 | 917504 | 0 | 0x13ce974804 |
| 0xf3b026 | 374 | 368 | 368 | 65536 | 0 | 0x1245607fc4 |
| 0xa3fb73 | 365 | 364 | 364 | 65536 | 0 | 0x11088033c8 |
| host:ucrtbase.dll+0xee3ec | 361 | 360 | 360 | 65536 | 0 | 0x1245627870 |
| 0xc14c6 | 266 | 358 | 31 | 917504 | 0 | 0x13ce9ac000 |
| 0xd3b7f | 359 | 351 | 351 | 65536 | 0 | 0x1405a08070 |
| 0x4a0ba | 442 | 332 | 19 | 51314688 | 0 | 0x10ad9fb8a0 |
| 0x4a1bf | 441 | 332 | 18 | 52887984 | 0 | 0x10dcafc030 |
| 0x4a1db | 393 | 298 | 19 | 67108864 | 0 | 0x10cc2fa680 |
| host:ucrtbase.dll+0xedece | 1148 | 284 | 276 | 131072 | 0 | 0x1253060000 |
| 0x11bd99 | 218 | 282 | 0 | 131072 | 0 | 0x13fee19020 |
| 0x1d4fbe | 256 | 255 | 255 | 65536 | 0 | 0x1108a00010 |
| 0x498cf | 338 | 246 | 23 | 48201280 | 0 | 0x110fbfb280 |
| 0x498fe | 317 | 232 | 18 | 52887984 | 0 | 0x13a17816d0 |
| 0xd9fd6 | 145 | 230 | 230 | 65536 | 0 | 0x1405a00ee0 |
| 0x499cc | 344 | 224 | 18 | 17825792 | 0 | 0x13fee0c4f0 |
| 0xd4877 | 213 | 213 | 213 | 65536 | 0 | 0x1400e02580 |
| 0xd5027 | 245 | 212 | 212 | 65536 | 0 | 0x13f9b029e0 |
| 0x499d8 | 291 | 209 | 13 | 16777216 | 0 | 0x13eefc7808 |
| 0xd39bb | 208 | 200 | 200 | 65536 | 0 | 0x1108a08880 |
| 0x499e0 | 376 | 198 | 49 | 16777216 | 0 | 0x1242fa16c8 |
| 0x48f3d | 241 | 198 | 9 | 12845056 | 0 | 0x10dc1ff8f0 |
| 0xafe13 | 311 | 188 | 172 | 196608 | 0 | 0x13ce938008 |
| 0x496f4 | 299 | 186 | 0 | 16777216 | 0 | 0x13eefdf290 |
| 0xf711b4 | 0 | 177 | 177 | 65536 | 0 | 0x13f9b0696c |
| host:bb-probe.exe+0x246b18 | 73 | 161 | 96 | 131072 | 0 | 0x124f653298 |
| 0x199c4f | 200 | 160 | 3 | 917504 | 0 | 0x13ce93c000 |
| 0x4a1f5 | 157 | 151 | 30 | 8912896 | 0 | 0x10851ffc80 |
| 0x18019 | 177 | 144 | 56 | 16056320 | 0 | 0x110e50cb48 |
| 0x135977 | 122 | 122 | 122 | 65536 | 0 | 0x1326b0b1c0 |
| 0xbbbe4 | 54 | 118 | 78 | 917504 | 0 | 0x13ce91814c |
| 0x4a16e | 122 | 91 | 2 | 52887984 | 0 | 0x10fa1fec40 |
| 0xd3a39 | 103 | 86 | 86 | 65536 | 0 | 0x1362e04f40 |
| 0xa3ef1b | 0 | 85 | 0 | 917504 | 0 | 0x13ce9bc000 |
| 0xa9bdae | 80 | 80 | 80 | 65536 | 0 | 0x1326b0aef4 |
| 0x861605 | 79 | 79 | 79 | 65536 | 0 | 0x124d0abeb9 |
| 0xcd61b | 75 | 75 | 0 | 917504 | 0 | 0x13ce933a34 |
| 0x4989a | 280 | 59 | 6 | 16121856 | 0 | 0x10fe4fb940 |
| 0x48ae5 | 92 | 57 | 2 | 8716288 | 0 | 0x10fcef97b0 |
| 0xf5a9 | 0 | 37 | 37 | 65536 | 0 | 0x13e160b4c0 |
| 0x48e62 | 46 | 32 | 1 | 67108864 | 0 | 0x10b57eb0f0 |
| 0xd3903 | 47 | 31 | 31 | 65536 | 0 | 0x135000ea80 |
| 0x1d864 | 30 | 30 | 22 | 131072 | 0 | 0x1245703a50 |
| 0x49883 | 41 | 30 | 0 | 16777216 | 0 | 0x10feed5320 |
| 0xc55c1 | 0 | 29 | 4 | 196608 | 0 | 0x13ce934018 |
| 0x4a300 | 37 | 29 | 1 | 16121856 | 0 | 0x10f8efee10 |
| 0x17cf1 | 37 | 28 | 21 | 67108864 | 0 | 0x124a94ffd8 |
| host:ucrtbase.dll+0xee4b3 | 35 | 28 | 1 | 89784320 | 19 | 0x1065500020 |
| 0x4a134 | 26 | 19 | 3 | 9371648 | 0 | 0x1243280ad8 |
| 0x86cf16 | 0 | 17 | 17 | 65536 | 0 | 0x12536352f4 |
| 0x9256d | 0 | 16 | 16 | 65536 | 0 | 0x124d9608e8 |
