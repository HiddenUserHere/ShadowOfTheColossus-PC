# BPE heap crash, guest side (B1, brief 4)

Status: diagnosis not closed. The evidence rules out heap metadata that is garbage in the simple sense
(the pointer is a well-formed block address in the game's heap). It does not yet say whether the page
was unmapped by the guest itself, or unmapped by the port for a moment. No CPU-side fix is shipped:
the evidence does not prove the cause, so `SOTC_BPE_FIX=2` adds diagnostics only (see section 6).

## 1. Facts from the crash logs

Logs: `user/crashes/*.log` (11 with `Crash at`). Full crash blocks read: 2026-11-02_02-20-34_0x3.log,
2026-11-02_01-52-34_0x3.log, 2026-11-02_00-23-39_0x3.log (the register dumps and stray-read lines of the
other crash logs were read too: 2026-11-01_19-29-21, 2026-11-01_23-03-03, 2026-11-02_02-20-06).

Every crash of this family has the same shape:

| log | rdi (heap object) | rsi (size) | r8 (rover) | fault address = r8+8 | state at query (after the fault) | thread |
|---|---|---|---|---|---|---|
| 2026-11-01_23-03-03 | 124560fd20 | 11cc0 | 10dc284c00 | 10dc284c08 | MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED, base 10dc200000, region 0x7c000 | 19388 |
| 2026-11-01_19-29-21 | (not extracted) | (not extracted) | (not extracted) | 1101a14708 | (stray line only) | 11192 |
| 2026-11-02_00-23-39 | 124560fd20 | f2b70 | 110c000010 | 110c000018 | MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED, base 110c000000, region 0x100000 | 25416 |
| 2026-11-02_01-52-34 | 124560fd20 | 3a780 | 10fe06e530 | 10fe06e538 | MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED, base 10fe000000, region 0x92000 | 6800 |
| 2026-11-02_02-20-06 | 124560fd20 | 2c30 | 10f80af800 | 10f80af808 | MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED, base 10f8000000, region 0x51000 | 28232 |
| 2026-11-02_02-20-34 | 124560fd20 | 2c30 | 10f7ea25f0 | 10f7ea25f8 | MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED, base 10f7e00000, region 0x5e000 | 25656 |

Other crashes of 11/01 at offset 0x490a0 (19-16-51, fault 10f56f73d8) and 0x9427f (19-40-55, 20-01-41,
20-37-10, 23-01-42: write to address 0, a different failure) are not analysed here.

Common to all six: the faulting instruction is a read at `[r8+8]`, with r8 16-byte aligned and inside
the game's heap mapping (10dc000000 .. 110c000000); the thread is a guest thread (fault in eboot code);
the Windows exception is a read access violation (ExceptionInformation[0]==0).

The `stray read state` line (page_manager.cpp:1557-1566) reads `state 0x1000 protect 0x4 type 0x40000`:
MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED. The "region" is the run from the faulting page to the next
boundary (for 02-20-34: page 10f7ea2000 + 0x5e000 = 10f7f00000, a 1 MiB boundary), so the answer
describes the page at the query, not the allocation.

The `[ERROR] Crash` line is printed by `fatal_exception` (src/probe.c ~833-858) and the
`Fault: read access to ...` line comes from the same function. The read is not handled by any other
path, because the tracker declines it (see section 4).

## 2. Disassembly (eboot = guest offset 0; image from out/boot-linked.bin)

Method: `scripts/link_modules.py` writes `BBPROBE5` header (8 + 6 qwords, then segments, bindings, names,
relocations) and the image LAST in the file, so the image is `raw[len(raw)-size:]` (here size 0x3c933d0,
image start 0x15e77d0). The eboot is at image offset 0 (`link()` maps the main ELF with base 0, and
`Entering original x86-64 code at guest offset 0xf120` matches entry 0xf120). Guest offset = image offset.
Slices were extracted with `python -I` (scratchpad `slice.py`), wrapped with
`llvm-objcopy -I binary -O elf64-x86-64 -B i386:x86-64`, and disassembled with `llvm-objdump -d`
(this llvm-objdump has no `-b`). Offsets below are absolute guest offsets.

Allocator function A (entry 0x48c80; caller of 0x49030 at 0x48d6b; return 0x48d70 = log frame #0):

```
048c80 pushq %rbp                     ; prologue
048cb0 movq $-0x1, %r14               ; lock value
048cc5 cmpq $0x0, (%r12)              ; r12 = a spin lock word
048cce lock cmpxchgq %r14, (%r12)     ; acquire
048cee movq -0x30(%rbp), %r14         ; r14 = heap object
048cf2 movq 0x28(%r14), %rdi
048d87 movq 0xc8(%rbx), %rcx          ; heap+0xc8
048da0 movq 0xd0(%rbx), %rsi          ; heap+0xd0
048d6b callq 0x49030                  ; the function that faults
048d70 movq %rax, %r13                ; <- log frame #0 (return address)
048de0 movq $0x0, 0xc8(%rbx)
```

Function B (entry 0x49030, the one that faults; prologue at 0x49030 `pushq %rbp`):

```
049030 pushq %rbp
049031 movq %rsp, %rbp
04903b movq 0xc0(%rdi), %r8           ; r8 = heap+0xc0 (the rover)
049042 testq %r8, %r8
049045 je 0x49077                     ; null: search the free list
049047 movq 0x8(%r8), %rax            ; <- FAULT: reads [r8+8], the size word of the rover block
04904b andq $-0x8, %rax
04904f cmpq %rsi, %rax                ; size >= requested (rsi)?
049052 jb 0x49077
049054 leal -0x1(%rdx), %ebx
049057 leaq 0x10(%r8), %r9
04905e leaq 0x10(%r8,%rbx), %rcx
049071 jae 0x491da                    ; the rover fits: take it
049077 movq 0x80(%rdi), %r10          ; heap+0x80: free-list sentinel
0490a0 movq -0x8(%rax), %rbx          ; (the second fault site of 2026-11-01_19-16-51)
```

Callers (the log rbp chain, checked against the code):

- 0x128f9 `callq *0xa0(%rax)` (vtable of rbx) -> returns 0x128ff = log frame #2.
- 0x47ee7 `callq *0xa0(%rax)` -> returns 0x47eed = log frame #1. Function 0x47e30 (prologue `pushq %rbp`)
  is the wrapper between them.
- 0x48d6b `callq 0x49030` -> returns 0x48d70 = log frame #0.

So the call chain is: `0x128f9 -> wrapper 0x47e30 (vcall +0xa0) -> allocator 0x48c80 -> rover test 0x49030`.

Heap object (rdi = 0x124560fd20 in all six logs; rdi is `this` of 0x49030): the offsets read or written
by these functions are:

| offset | use (file offsets of the instruction) |
|---|---|
| +0x18 | int, read at 0x48da7 (`movl 0x18(%rbx),%ecx`) |
| +0x28 | pointer, read at 0x48cf2 (`movq 0x28(%r14),%rdi`) |
| +0x80 | free-list sentinel (0x49077 `movq 0x80(%rdi),%r10`; loop 0x4909d-0x490b4 walks `-0x8(%rax)` size words) |
| +0xb8 | int, written at 0x481fb (`movl %esi,0xb8(%rdi)`) |
| +0xc0 | ROVER (written at 0x47e17 `movq %rdx,0xc0(%rdi)`, 0x48201 `movq %r8,0xc0(%rdi)`, 0x491dd `movq $0,0xc0(%rdi)`; read at 0x4903b) |
| +0xc8 | written at 0x47e1e, 0x4836b (zeroed), read at 0x48d87 |
| +0xd0 | high-water mark style counter (cmp at 0x48da0 and 0x49xxx) |
| +0xd8/+0xe0 | byte counters (addq, cmpq at 0x48f83-0x48f9f) |
| +0xe8 | count (`incq 0xe8(%rbx)` at 0x48f78) |

The rover is therefore an allocator-owned cache written by the allocator itself. Its value is a block
base, not a user pointer: the user pointer is block+0x10 (`leaq 0x10(%r13,%r15)` at 0x48d70 region),
the size word is at block+8 (`movq 0x8(%r13),%rcx; andq $-8`), and a new segment starts with a
16-byte head (rover 110c000010 = segment 110c000000 + 0x10).

Field of the fault: `[heap+0xc0] + 8` = the size word of the rover block. The bad pointer is therefore
the rover itself (r8), not an inner metadata word.

## 3. Garbage pointer or valid memory? (step 2 of the brief)

Evidence for "the pointer is a plausible block address":

- All six r8 values are 16-byte aligned and lie in one 512 MiB window (10dc000000..110c000010), the
  same mapping the game uses for its heap; the fault is always at r8+8 with the same instruction.
- A random or GPU-written 64-bit value would not land in that window six times in a row, nor at a
  segment head (110c000010) and at aligned positions.
- The rover test in function B is the allocator's own fast path; its value is an address it wrote.

Evidence against "the page is simply valid and the port made it unreadable for good":

- The page was accessible at the query (MEM_COMMIT, PAGE_READWRITE). It is only "valid" if the access
  faulted during a window in which the page was not accessible. Section 4 shows which windows exist.

Evidence against "the heap metadata is corrupt in a simple way": nothing in the logs shows the value of
[r8+8] or [r8] (the faulting read never completes). The minidump (`*_bb-probe.dmp`, MiniDumpNormal) has
thread stacks but no heap memory, so it cannot show the block.

Verdict: the pointer is a well-formed heap block. Whether the game (or the port) unmapped the block
before the access, or whether the rover was corrupted to point at a block that was never mapped, is
not decided by these logs. The B1 question "garbage or valid" reduces to: was the page unmapped at
the moment of the fault, and by whom?

## 4. Why VirtualQuery reports committed read-write after the fault

Three things in the code produce a read fault on a page that is RW a moment later.

1. The read fault itself: an access to a page is an access violation when the page is MEM_RESERVE
   (a placeholder, PAGE_NOACCESS) or PAGE_NOACCESS. A read of a page that is RW does not fault.
2. The memory runtime changes the mapping in windows, under its write lock:
   - `carve_host` (src/runtime_memory.c:685-~860) unmaps the views it splits (line 779,
     `view_unmap` -> `UnmapViewOfFile2(..., MEM_PRESERVE_PLACEHOLDER)`), leaving placeholders
     (MEM_RESERVE, PAGE_NOACCESS), and maps the kept parts again RW (`view_map`, line 476), then restores
     the saved protection (line 831). Between the unmap and the remap a read faults; after the remap the
     page is MEM_COMMIT / PAGE_READWRITE / MEM_MAPPED, which is exactly the state in the logs.
   - `view_map` with PAGE_NOACCESS maps RW first and protects after (line 482-486): the same pattern in the
     other direction.
3. Those windows are only short if nothing waits on them. carve_host suspends the other guest threads
   for a split with kept parts (src/runtime_memory.c:773-777,
   `if ((keep_left || keep_right) && !nostop && carve_suspend_enabled())`, default on:
   SOTC_CARVE_SUSPEND unset, SOTC_CARVE_NOSTOP unset). A guest thread stopped by SuspendThread cannot
   fault. So a split with kept parts cannot explain a guest read fault, unless a guest thread is not in
   the suspended set.
   The unsuspended case is a full unmap with no kept part: `keep_left==keep_right==0` skips the suspend
   (condition at line 773) and also skips carve_log (split_start is 0). A guest thread that reads that range while
   another guest thread unmaps it faults, and the page is RW again once the game maps it back.

What the state at the query tells: the query is made from page_manager.cpp:1557-1566 after
`NoteStrayFault` and after the tracker declined. In native mode `TracksGuestPages()` is false
(gpu/sotc/native/native.h:39-41), so the tracker protects nothing and `HandleGuestFault` returns false
for reads. So the page was not protected by the tracker: the access was to memory without a view
(placeholder) at the instant of the fault. The query came later, after the mapping came back.

Why the window is so short: the query runs within the same exception dispatch (microseconds). The
unmap and the remap that the game or the port issues must be back to back for the query to see RW.
That fits a remap pattern (the comment above `view_chunk()` in src/runtime_memory.c says the game remaps
64 KiB pages with sceKernelBatchMap while an area streams), and it fits a rare race (1 run in 4).

Not yet proven: which thread unmapped the range, and whether the game itself ever unmaps a range that
still holds heap blocks (on a PS4 that access would fault too, so the port would have to be creating the
state). The logs do not record map or unmap calls (SOTC_CARVE_LOG=1 prints only split carves, and only
with kept parts).

## 5. Decision (step 2 of the brief)

Not decided from the logs. Two readings remain, and the next run separates them:

- A. Port-induced: the port unmapped or remapped a range that the game still uses (a whole-view unmap
  with no suspension, or a placeholder window). Then the fault state at the fault is MEM_RESERVE and the
  mapping history shows a port unmap of that range just before the fault.
- B. Game-induced or corruption: the rover points at a block the game (or GPU writes, see B2) left
  unmapped. Then the state at the fault is MEM_RESERVE too, but the mapping history shows a game unmap
  (sceKernelMunmap / sceKernelBatchMap) and the page does NOT become accessible within the wait, or it
  does so only after a game map of that address.

## 6. Diagnostics added (SOTC_BPE_FIX=2, src/probe.c)

Observe-only, no change in the fault handling. Unset or any other value: nothing is queried.

- `vectored_fault_impl` (src/probe.c, first lines): for a read access violation in game code on a guest
  thread, the page state at the fault is captured (VirtualQuery) before any handler runs.
- Just before the fatal path (`bpe_observe_fault`): if the fault reaches the fatal path, the handler waits
  up to 50 ms for the page to become accessible (VirtualQuery in a loop with Sleep(0)), reads the block's
  two words at [addr-8] and [addr] when accessible (ReadProcessMemory), and prints one line:

```
SOTC BPE read fault <n>: addr <addr>; state at the fault: state <s> protect <p> allocation base <b> region <r> type <t>; accessible|still inaccessible after <ms> ms, state now <s> protect <p>; [addr-8] <w0> [addr] <w1>; rdi <heap> r8 <rover>
```

The first 64 faults and every 100th are printed. The fault then continues to `fatal_exception` as before,
so the process still ends with the same report.

Counters: `bpe_faults`, `bpe_accessible_after_wait`, `bpe_still_inaccessible` (module statics; not printed
yet, the per-line output is enough for this run).

What one run with `SOTC_BPE_FIX=2` must show (the coordinator runs it):

1. The `state at the fault` field of the BPE line:
   - `state 0x2000` (MEM_RESERVE) or `protect 0x1` (PAGE_NOACCESS) at the fault: the access hit a
     placeholder or a no-access page. Continue with item 2.
   - `state 0x1000 protect 0x4` at the fault: the page was RW at the fault, so the fault was not a window
     of unmapping. Then the fault has another cause (a stale view or an error in the fault path): report it.
2. `accessible after N ms` with N small (under 1 ms): the window was short and closed; the next question
   is the mapping history (who unmapped the range, see item 4).
   `still inaccessible` for all 50 ms: the rover points at memory that stays unmapped, so the rover is
   wrong (reading B in section 5). The heap metadata question then moves to B2.
3. The block words: a sane size word at [addr-8] (16-byte multiple, below the heap bound, flags in the low
   bits) after the wait suggests a valid block; garbage there suggests the metadata is wrong too.
4. Mapping history of the range. The current logs do not record map/unmap calls. Set `SOTC_CARVE_LOG=1`
   (prints split carves with range, time and "threads stopped"), and add the suggested diagnostic below
   to src/runtime_memory.c (not part of this change: it is not in the ownership map).

Suggested diagnostic for src/runtime_memory.c (for the coordinator, not applied):

```c
/* host_unmap (src/runtime_memory.c:905) and host_map (line 869): with SOTC_MAP_LOG=1, print the range
 * when it overlaps 0x10dc000000..0x110c000000, the thread id and the tick (GetTickCount64). Then the
 * fault line of SOTC_BPE_FIX=2 can be matched to the last unmap of its page. */
```

Interpretation table for the run:

| fault state | after-wait | last unmap of the page (SOTC_MAP_LOG) | reading |
|---|---|---|---|
| MEM_RESERVE | accessible < 1 ms | port unmap just before | A: port-induced window |
| MEM_RESERVE | accessible after a game map | game unmap just before | A/B: the game and the port raced on it |
| MEM_RESERVE | still inaccessible | game unmap, no re-map | B: rover points at unmapped memory (heap corruption) |
| PAGE_NOACCESS (commit) | accessible | none | protection window (not the tracker in native mode: find who set NOACCESS) |

## 7. What was not changed, and why

No CPU-side fix behind `SOTC_BPE_FIX=1`. The brief allows one only if the evidence proves the cause.
The evidence shows a well-formed block and a transient read fault. Retrying the access after a wait
would hide the unmap: if the game really dropped that block, the retry would read another mapping's
bytes. That needs the run above first.

Not touched (ownership): src/runtime_memory.c (carve suspend, view_unmap, the map log), gpu/shadps4
page_manager.cpp (the stray-read path, see B2), video_core/buffer_cache/bpe_heap_guard.cpp (B2).

## 8. Check run

Syntax check (the compile entry for src/probe.c in out/gpu/compile_commands.json, with -std=c11
-Wall -Wextra -Werror -fsyntax-only, the worktree include paths first):

```
C:/msys64/clang64/bin/clang.exe -DBB_MIMALLOC -DMINIZ_STATIC_DEFINE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -IC:/Users/ptr/Documents/sotc-pc/.claude/worktrees/agent-aa56cc2219f562cb4/gpu/.. -IC:/Users/ptr/Documents/sotc-pc/.claude/worktrees/agent-aa56cc2219f562cb4/gpu/../src -IC:/Users/ptr/Documents/sotc-pc/gpu/third_party/magic_enum/include -IC:/Users/ptr/Documents/sotc-pc/gpu/third_party/magic_enum/include/magic_enum -IC:/msys64/clang64/include/vma -IC:/Users/ptr/Documents/sotc-pc/gpu/third_party/miniz -IC:/Users/ptr/Documents/sotc-pc/out/gpu/third_party/miniz -IC:/Users/ptr/Documents/sotc-pc/gpu/third_party/sirit/src/../include -IC:/Users/ptr/Documents/sotc-pc/gpu/third_party/sirit/externals/SPIRV-Headers/include -std=c11 -Wall -Wextra -Werror -fsyntax-only C:/Users/ptr/Documents/sotc-pc/.claude/worktrees/agent-aa56cc2219f562cb4/src/probe.c
```

Output: exit 0, no diagnostics.

## 9. Mapping history (SOTC_MAP_LOG=1, src/runtime_memory.c) and the run plan

What it records (Windows build only): every change of a guest range that overlaps
[0x10dc000000, 0x110d000000): `map` (host_map, view_map), `unmap` (host_unmap, view_unmap), `protect`
(host_protect, view_map's protection, protect_views, the carve restores), and `placeholder` (the
VirtualFree calls that split or coalesce placeholders: map_fine, carve_host, host_map's piece cuts).
Each entry: op, host thread id, the new protection (protect/map), the range, a QPC timestamp in ns, and
the caller: the address the instrumented host function returns to (__builtin_return_address(0) at the
entry of that function, i.e. the function that called it). Ring of 4096 entries, only in-range changes
are stored, so the ring holds the last 4096 changes of the heap range. Lock-free (see the comment above
MAP_LOG). With the switch off, each instrumented function costs one cached branch (`map_log_on()`).

Dump (probe.c, `bpe_observe_fault`, printed with the BPE line): the entries overlapping the fault page,
newest first, at most 32, with `age` in microseconds relative to the fault (positive = before the
fault, negative = after it), op, tid, the thread description (GetThreadDescription, or "unnamed"),
range, protection, and caller. If the fault page is MEM_RESERVE or PAGE_NOACCESS at the fault, the
last 16 entries overlapping its 1 MiB block are printed as well. The faulting thread id and name are on
their own line.

Limits:
- The caller is the host function's caller (the runtime's import implementation for the game's
  call: sceKernelMunmap/MapFlexibleMemory/BatchMap or the port's own split). It does not give the guest
  return address: the guest's stack is not walked (cheap walk not added).
- Thread names come from GetThreadDescription; the port's guest thread names (`guest thread '<name>'`,
  src/runtime_thread.c:561) are not mapped from the tid. If the names print as "unnamed", the runtime
  would need a tid-to-name table (not done here: runtime_thread.c is not in the ownership).
- Chunked mode (SOTC_COMMIT_CHUNKS, runtime_memory_chunks.c) is covered only at the view_map/view_unmap
  level, because the logs sit in runtime_memory.c; the chunk file itself is not instrumented.
- Linux build: not instrumented (the Windows block only).

Run plan (one run; the coordinator runs the game, about 1 run in 4 crashes at 11-33 s):

    SOTC_BPE_FIX=2 SOTC_MAP_LOG=1 SOTC_CARVE_LOG=1   (plus the usual switches; SOTC_THREAD_IDS=1 is optional)

Read in the crash log: the `SOTC BPE read fault` line (state at the fault), the `faulting thread` line,
the `SOTC MAP LOG` blocks. Decision:

1. Fault state MEM_COMMIT PAGE_READWRITE (not reserve, not NOACCESS): the fault is not a window of the
   port's mapping. Report the path; the MAP LOG shows whether the page was changed at all.
2. Fault state MEM_RESERVE or PAGE_NOACCESS, and the newest entry overlapping the page is an `unmap` (or
   a `placeholder`) with a positive age of microseconds to a few milliseconds:
   - the caller is host_unmap's caller (a game import) and the tid is another guest thread: a game
     unmap raced the faulting thread. The game unmapped a range the heap still points to (reading B,
     heap metadata wrong, B2 path). Check whether `accessible after` is short (the remap came back).
   - the caller is view_unmap, carve_host or host_map (the port's split): a port window (reading A). A
     port split suspends the other guest threads, so a faulting thread that is not suspended is itself a
     finding: note its tid and whether it is in the suspend set.
3. Fault state PAGE_NOACCESS (commit): the newest entry is a `protect` with prot 0x1 from a caller:
   find that caller (who made the page inaccessible); the readback/tracker is off in native mode.
4. No entry overlaps the page within the ring: the ring is too short for the wait (ages of the printed
   entries show the coverage) or the page was mapped by a path not logged (chunks, or before the run).
5. Entries with NEGATIVE ages (after the fault) on the page: a remap after the fault explains the RW state
   at the query. Their tid and caller name the thread and the call that restored the page.

Record the outcome in this section (one line per decision above) after the run.

## 10. Resolution (2 Nov, run with SOTC_BPE_FIX=2 SOTC_MAP_LOG=1)
The run that crashed printed:
- state at the fault: `state 0x2000 protect 0x0 type 0x20000` (a placeholder), accessible again 0.001 ms later
  (`state now 0x1000 protect 0x4`). The block header words were sane (`[addr-8] 0x10f860e300`, `[addr] 0xaaba0`).
- `SOTC BPE NATIVE no metadata overlap within 1 MiB of the fault (0 overlaps since start)`: no in-place GPU write
  touched heap metadata. The GPU-side hypothesis (B2) is killed.
- map log, oldest first, relative to the fault: thread "BPE JobWorkerTh" unmapped [0x10f8600000,0x10f8800000)
  76 us before (host_unmap from unmap_locked), split placeholders, and mapped the kept part
  [0x10f8600000,0x10f8710000) back 21 us after (carve_host). The faulting thread was the main thread.

Cause: carve_host stops the other guest threads while it splits a live view
(runtime_thread_suspend_others), but that function skips threads without an OS handle, and the main thread's
record (runtime_thread_current, then runtime_thread_attach_main) never got one: only threads started through
host_start opened theirs. The main thread runs the game's main loop and the heap allocator, so it read the heap
inside the split window and faulted at eboot+0x49047.

Fix (src/runtime_thread.c, runtime_thread_attach_main): the main thread opens its OS handle under `lock`, so the
split stops it too. No switch: it is what the suspension was meant to do. After the fix, 6 Lab runs in a row
passed (before: about 1 run in 4 crashed).
