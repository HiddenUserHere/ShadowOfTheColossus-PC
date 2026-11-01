# 08 - Returning commit charge and RAM for freed ranges of a section mapped twice

## Question

The port keeps all guest memory in one pagefile-backed section (`src/runtime_memory.c`), mapped twice:
a backing view (`MapViewOfFile3`) and guest views that replace placeholders
(`VirtualAlloc2(MEM_RESERVE_PLACEHOLDER)` + `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`). With
`SOTC_COMMIT_ON_DEMAND=1` the section is `SEC_RESERVE` and pages are committed through the backing view
with `VirtualAlloc(MEM_COMMIT)`. Freeing is the problem: `VirtualFree(MEM_DECOMMIT)` on section views
fails with error 87, so freed guest memory keeps its commit charge until exit.

This report measures every candidate API on this machine for a freed range of such a section.

## Environment

- Windows 11 Pro, build 10.0.26300. 31.9 GiB RAM, pagefile 2720 MB. The system was busy during the
  runs (system commit about 21.5 GB), so system-wide deltas carry a noise floor of about +/-5 MiB.
- Token privileges: `SeLockMemoryPrivilege` is **not** held (relevant to AWE, see below).
- Compiler: clang 23.1.3 (CLANG64), linked with `-lpsapi -lmincore -lonecore`.
- Test program (not part of the repo): `C:\Users\ptr\AppData\Local\Temp\claude\C--Users-ptr-Documents-sotc-pc\5c619f39-4d55-404e-9cbb-38a52993d236\scratchpad\commit_options\memtest.c`
  (built as `memtest.exe` in the same folder).

## Measurement method

- **Commit charge (system-wide):** `GetPerformanceInfo().CommitTotal`. This is the only measure that
  sees section commit: `PrivateUsage` (process) excludes section views (it read 2.7 MiB while 256 MiB of
  section pages were committed and touched).
- **RAM:** `GetPerformanceInfo().PhysicalAvailable` (standby counts as available) and
  `QueryWorkingSetEx` valid bits for the guest range (residency of the process).
- **Job Object limits:** a control was run first. With `JOB_OBJECT_LIMIT_PROCESS_MEMORY` or
  `JOB_OBJECT_LIMIT_JOB_MEMORY` set to 384 MiB, private `VirtualAlloc(MEM_COMMIT)` is refused at
  382 MiB (`ERROR_COMMITMENT_LIMIT` 1455). So the job limit works for private memory, and the section
  results below are a valid negative.
- **Content check:** after each op, a byte of the freed range is read through the guest view (only for ops
  that leave the range accessible).
- Freed range: 64 MiB single view unless noted. Guest views are 64 MiB, so a range cannot span two views
  for the ops that fail on spanning (see the table).

## Results

### Option by option

| # | Operation on a section view | Result code | Commit returned? | RAM returned? | Notes |
|---|---|---|---|---|---|
| 0 | `VirtualFree(MEM_DECOMMIT)` on private memory (baseline) | 0 | **yes** (-128.0 MiB) | yes | The mechanism the port wants; does not apply to section views |
| 1a | `VirtualFree(MEM_DECOMMIT)`, guest view, 64 and 256 MiB | 0x57 (ERROR_INVALID_PARAMETER) | no (no change) | no | Confirms the error 87 in the port |
| 1b | `VirtualFree(MEM_DECOMMIT)`, backing view, 64 and 256 MiB | 0x57 | no | no | Same |
| 2 | `DiscardVirtualMemory`, guest view, single view (64 MiB), offset 0, 64 MiB offsets 64 and 128 | 0 | **no** (0.0 to +0.1 MiB) | **yes** (avail +51.6, +51.3, +57.6 MiB per 64 MiB); residency 0 | Byte read back as **0** (contents discarded) |
| 2b | `DiscardVirtualMemory`, guest view, 256 MiB (four views) | 0 | no (+1.0 MiB) | yes (avail +251.7 MiB) | Cost 17 to 83 ms per 256 MiB (noisy) |
| 2c | `DiscardVirtualMemory`, guest view, 128 MiB spanning two views | 0x57 | no | no | Range may not span view boundaries |
| 2d | `DiscardVirtualMemory`, backing view, 64 and 256 MiB | 0 | no | residency 0 only (avail change within noise) | Byte read back as **1** (contents kept); behaviour differs from the guest view |
| 3 | `OfferVirtualMemory` + `ReclaimVirtualMemory`, guest or backing, 64 MiB | 0 / 0 | no (0.0 to +1.9 MiB, noise) | no pressure applied, so not forced | Reclaim returns success with content intact (no discard under no pressure); cost 21 to 30 ms per 64 MiB offer+reclaim |
| 3b | `OfferVirtualMemory`, guest view, 128 MiB spanning | 0x57 | no | no | Spanning fails |
| 4 | `VirtualAlloc(MEM_RESET)`, guest or backing, 64 and 256 MiB | 0 | no | residency 0; avail within noise | Byte read back as **1**; `MEM_RESET` gives no RAM gain beyond what discard does |
| 4b | `VirtualAlloc(MEM_RESET)`, guest view, 128 MiB spanning | 0x1E7 (ERROR_INVALID_ADDRESS) | no | no | Spanning fails |
| 4c | `MEM_RESET` then `MEM_RESET_UNDO`, guest view | 0 | no | residency 0 | Undo succeeds after reset |
| 5 | `NtFreeVirtualMemory(MEM_DECOMMIT)`, guest view, single 64 MiB | 0xC000001B (STATUS_UNABLE_TO_DELETE_SECTION) | no | no | Status codes checked in `ntstatus.h` |
| 5b | `NtFreeVirtualMemory(MEM_DECOMMIT)`, guest view, 128 MiB spanning | 0xC000001A (STATUS_UNABLE_TO_FREE_VM) | no | no | |
| 5c | `NtFreeVirtualMemory(MEM_DECOMMIT)`, backing view | 0xC000001B | no | no | |
| 6 | `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` on a guest view | 0 | **no** (0 MiB) | residency -> 0 | Unmapping a view does not release the section's commit |
| 7 | `EmptyWorkingSet` (process) | 1 | no | residency 256 -> 0; avail -0.7 MiB | Physical only; pages go to standby |
| 8 | AWE: `AllocateUserPhysicalPages` | 1314 (privilege not held) | - | - | `AdjustTokenPrivileges(SeLockMemoryPrivilege)` returns 1300 (not held). Not testable without a policy change; see option 8 below |
| 9 | Job Object limit (process or job memory) with a section | - | section commit **not counted** | - | Fill reached the full 1024 MiB under both kinds, for every op including `none`. Private commit is counted (control). A job limit cannot prove section charge returns |
| 10 | **Per-chunk section, unmap both views, release placeholder, `CloseHandle(section)`** | 0 | **yes, exactly** (-16.0, -64.0, -256.5 MiB at 16, 64, 256 MiB) | yes (avail +2.7, +48.6, +241.5 MiB) | The only option that returns commit. Unmapping alone does not; the handle close is the step that returns it. See section below |

Option 6 results from the single-view run: `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` removed the view
(valid -64 MiB) and returned no commit (+1.4 MiB, noise).

### Per-chunk sections (option 10), with chunk-size effect

Structural cost per chunk (create section, backing view, placeholder, guest view, unmap, release, close,
no commit): 12.2 us (16 MiB), 28.0 us (64 MiB), 29.5 us (256 MiB). Negligible.

Commit and touch for 1 GiB, for reference: 257 us/MiB (16 MiB chunks), 257 us/MiB (64), 256 us/MiB (256).
This includes page-fault zeroing and is not the commit call alone.

Free cost per chunk (unmap guest, unmap backing, close section, release placeholder):

| Chunk | Free per chunk | Free per MiB | Commit after the chunk's close |
|---|---|---|---|
| 16 MiB | 1406 us | 88 us | -16.0 MiB (exact) |
| 64 MiB | 5658 us | 88 us | -64.0 MiB (exact) |
| 256 MiB | 22929 us | 90 us | -256.5 MiB (exact) |

Step-by-step (chunk 0 of 16, 64 and 256 MiB) shows the commit drops **only** at `CloseHandle(section)`:
unmapping the guest view and the backing view changes commit by 0 MiB. Full release of all 64 chunks of
16 MiB returned commit to +7.9 MiB of the start (noise); 16 chunks of 64 MiB returned +1.0 MiB.

Chunk size only changes granularity and the number of handles and placeholders. Per-MiB free cost is the
same (about 88 to 90 us), so the cost is in releasing pages, not in the chunk machinery.

### Option 8: AWE (AllocateUserPhysicalPages / MapUserPhysicalPages)

- The token does not hold `SeLockMemoryPrivilege`. `AdjustTokenPrivileges` returns 1300, and
  `AllocateUserPhysicalPages` fails with 1314.
- The privilege has to be granted by policy ("Lock pages in memory") and the user must sign in again.
  Changing policy is outside what this task may do, so the mapping cannot be tested here.
- Not verified: whether one physical page can be mapped at two virtual addresses. The documentation
  suggests a page maps to one address at a time, which would rule out the two-view design, but this
  has not been measured.
- Verdict: **not feasible** for the port without a policy change, and even then it is unverified for
  the two-view design. It also needs locked, non-paged physical memory, which conflicts with the
  port's goal of letting freed pages go.

### Option 7: other APIs considered

- `EmptyWorkingSet` and `SetProcessWorkingSetSizeEx` trimming: physical residency only; pages move to
  standby. No commit change (measured). Useful only as a lever for RAM pressure, not for commit.
- `MEM_EXTENDED_PARAMETER` on `CreateFileMapping2` / `VirtualAlloc2`: NUMA preference and large
  pages. Large pages are ruled out (4 KiB tracking). NUMA does not change commit. Not measured.
- Memory partitions and `MEM_RESET` variants: not exposed to user code in a way that changes commit
  (`MEM_RESET` measured above).

## Ranked table

Ranking is by whether the option returns commit (the goal), then by RAM, then by cost.

| Rank | Option | Returns commit? | Returns RAM? | Cost (measured) | Risks | Windows |
|---|---|---|---|---|---|---|
| 1 | Per-chunk section, unmap both views, release placeholder, `CloseHandle(section)` | **Yes, exact** | Yes | 12 to 30 us structural per chunk; free about 88 us per MiB | Every view and handle must be gone; any open handle or import keeps the charge. Placeholder split and coalesce bookkeeping. Chunk granularity vs. VA fragmentation | Win10 1803+ (placeholders); tested on 26300 |
| 2 | `DiscardVirtualMemory` on a single guest view | No | Yes (avail +51 to +252 MiB) | 5 to 18 ms per 64 MiB | Single view only; contents read back as zero (fine for freed memory); spanning fails | Win8.1+; tested on 26300 |
| 3 | `OfferVirtualMemory` / `ReclaimVirtualMemory` | No | Only under memory pressure (not forced here) | 21 to 30 ms per 64 MiB offer+reclaim | Content may be discarded under pressure; cost is high | Win8.1+ |
| 4 | `MEM_RESET` / `MEM_RESET_UNDO` | No | Residency only | 1 to 3 ms per 64 MiB | Spanning fails (487); contents kept on backing | MEM_RESET_UNDO: Win8+ |
| 5 | `EmptyWorkingSet` | No | Residency only (standby) | Fast | Physical only | Long-standing (psapi) |
| 6 | `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` alone | No | Residency only | Fast | Commit stays until handle closed | Win10 1803+ |
| 7 | `VirtualFree(MEM_DECOMMIT)` on section views | No (fails 0x57) | No | - | Fails on backing and guest | - |
| 8 | `NtFreeVirtualMemory(MEM_DECOMMIT)` on section views | No (0xC000001A/B) | No | - | Undocumented; fails | - |
| 9 | AWE | Not testable (privilege) | - | - | Privilege not held; two-view mapping unverified | Long-standing; needs SeLockMemoryPrivilege |

## Recommendation

1. **Adopt per-chunk sections, closed on free (option 1).** This is the only option that returns commit
   on this machine, and it matches the plan of the other agent ("one section per 64 MiB chunk, close the
   section when the chunk is free"). The measurements show commit drops exactly by the chunk size, only at
   `CloseHandle`. Use **64 MiB chunks**: they match the existing 64 MiB guest view granularity, the
   structural cost is about 28 us, and free cost is about 88 us per MiB. 16 MiB is also fine for finer
   granularity (64 sections per GiB). Avoid 256 MiB (coarse frees).
2. **Use `DiscardVirtualMemory` on guest single views (option 2) as a RAM lever, not a commit lever.** It
   returns RAM immediately while keeping the chunk's commit. It is useful for idle chunks under memory pressure,
   and must be called per view (not spanning).
3. **Do not pursue** `VirtualFree(MEM_DECOMMIT)`, `NtFreeVirtualMemory(MEM_DECOMMIT)`, MEM_RESET,
   OfferVirtualMemory, or AWE for commit return. They either fail or do not reduce commit.
4. **Job Object limits cannot verify section commit.** Use `GetPerformanceInfo().CommitTotal` deltas
   (system-wide) and treat changes under about 5 MiB as noise.

## Vulkan VK_EXT_external_memory_host

Not tested here (no Vulkan import was run). Reasoning from the specification and from the measurements:

- An imported host allocation must stay valid for the life of the `VkDeviceMemory`. The driver may pin the
  pages, which keeps their references alive. An open reference (handle, view, or pin) keeps the commit
  charge, as the measured negative results show for open views and handles.
- Therefore a chunk that the GPU imported must **not** be closed until the import is released. Closing it
  while imported would leave commit charged (or leave the GPU reading freed pages).
- Detection, in order of reliability:
  1. Bookkeeping (required): record, per chunk, every `VkDeviceMemory` whose host pointer lies in it
     (`VkImportMemoryHostPointerInfoEXT`). Close the chunk only when the count is zero, after
     `vkFreeMemory` and a fence wait for the GPU work that used it.
  2. Runtime canary (debug): after `CloseHandle`, check that `CommitTotal` dropped by the chunk size. If it
     did not, the chunk is still referenced (by a view, a handle, or an import). Log it.
- The import alignment must satisfy `vkGetMemoryHostPointerPropertiesEXT` (`minImportedHostPointerAlignment`).
  64 MiB chunks aligned to 64 KiB are safe in practice; verify with the query.
- The port currently refuses `SOTC_COMMIT_ON_DEMAND` with `SOTC_HOST_IMPORT` (`runtime_memory.c` line ~837),
  because the whole guest memory is imported and stays committed. Per-chunk sections only work with
  per-chunk imports (one `VkDeviceMemory` per chunk, or sub-allocations that never span a chunk).
  Test this with a real import before enabling both.

## Open items

- Not measured: the Vulkan import pin behaviour (needs a Vulkan device and `VK_EXT_external_memory_host`).
- Not measured: AWE two-view mapping (needs `SeLockMemoryPrivilege`).
- Not measured: `OfferVirtualMemory` under real memory pressure (reclaim returning ERROR_BUSY after a discard).
- The free cost (about 88 us per MiB) is for release only; a real workload also pays for the commit
  and touch (about 257 us per MiB), which the per-chunk design does not change.
