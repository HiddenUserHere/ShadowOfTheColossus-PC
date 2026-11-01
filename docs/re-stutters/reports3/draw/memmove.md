# Draw thread: ucrtbase memmove / memcpy / memcmp call sites

Source: `drawprof-base.txt` (20 s, 1 ms sampling, 11,446 samples), binary `out/gpu/bb-probe.exe`
(built 03:56, before the current working-tree edits; line numbers below are from that build and
can drift from the current sources; function names are the anchor).

## Method

The raw dump has no call graph, but the port's own `memcpy`/`memmove`/`memcmp` calls go through
import thunks that jump to the CRT (`jmp __imp_memmove` etc.). That gives an exact attribution:

1. `llvm-objdump -d` on `bb-probe.exe`: 939 call sites of the `memmove` thunk (0x140688fd0),
   1,651 of `memcpy` (0x140688430), 210 of `memcmp` (0x140688420).
2. A `call` pushes the return address `site + 5`. memmove/memcpy/memcmp are leaves, so the
   dump entry for `bb-probe.exe+(site+5-0x140000000)` has an inclusive count equal to the
   samples spent inside that call from that site. (Checked: the 36 matching sites sum to
   1,556 samples; the ucrtbase self total is 1,576, so nearly all CRT samples are covered.)
3. `llvm-symbolizer --inlines` and `llvm-nm` name the sites and their owners.

Note: `pair::operator=` (14.6% inclusive) is not a copy source. It is a folded label whose
inclusive time is its callees (the staging copies below), and no CRT leaf sits under it.

## Ranked copy sources (draw thread, samples of 11,446)

| # | Copy (CRT leaf) | Owner (call path) | Samples | % |
|---|---|---|---|---|
| 1 | memcpy, guest memory -> staging (`dest = staging.mapped`) | `Core::GuardedGuestCopy` <- `CopySparseSerial` <- `MemoryManager::CopySparseMemory` (`gpu/shim/bbgpu.cpp:217`, `:241/:248`) | 628 | 5.5 |
| 1a | of which, CopySparseMemory callers | `BufferCache::ObtainBufferForImage` (texture guest copy into staging, `buffer_cache.cpp` ~6327 and the inlined `ObtainGuestCopyForImage` ~6295) | 407 | 3.6 |
| 1b | | `BufferCache::SynchronizeMemoryFull` bulk copy to staging (~7412/7417; hash-path copy at ~7369) | 284 | 2.5 |
| 2 | memcmp, page compare of table vs source | `ReplaceChangedPages` (`vk_shader_hle.cpp:380`) | 131 | 1.1 |
| 3 | memmove, guest write-back (`dest = guest backing`) | `runtime_memory_write_backing` / `write_backing_fast` (`src/runtime_memory.c:1821-1834`) | 156 | 1.4 |
| 4 | memmove, `vector<u32>::assign` of the flattened user data | `Liverpool::ExecuteBackOp` line ~1132 (`rt.flattened_ud_buf.assign(stage.flat...)`) | 103 (+ inner call 131 incl.) | 0.9-1.1 |
| 5 | memcpy, `BpeHeapGuard::Read` (guest backing read) | `VideoCore::BpeHeapGuard::Read` (`bpe_heap_guard.cpp`) | 129 | 1.1 |
| 6 | memcpy, table build of the HLE source | `BuildTable` (`vk_shader_hle.cpp:410`) plus doubling at 413 | 89 + 6 | 0.8 |
| 7 | memmove, interval shifting | `IntervalList<Interval>::AddUnion` | 38 + 3 | 0.4 |
| 8 | memcpy, descriptor data recorded for push descriptors | `CmdPushDescriptorSet` (`vk_recorder.cpp:403/412`) | 29 + 23 | 0.5 |
| 9 | memcpy, stream buffer upload | `StreamBuffer::Copy` (`buffer.h:172`) | 29 | 0.3 |
| 10 | memcpy, register chunks | `Liverpool::ExecuteBackOp` line ~1121 | 44 | 0.4 |
| 11 | memcpy, sparse read (TryCopySparseMemory) | `TryCopySparseMemory` <- `DrawIndirect` and `TryWriteThroughFromCpu` | 15 + 10 | 0.2 |
| 12 | memcpy, push constants | `CmdPushConstants` (`vk_recorder.h:193`) | 7 | 0.1 |
| 13 | memcmp, RenderState | `Scheduler::BeginRendering` (`vk_scheduler.h:70`) | 14 | 0.1 |
| 14 | memcpy, mip tail | `ImageDesc::InitMipTail` (`texture_cache.cpp:940`) | 14 | 0.1 |
| 15 | copy of ImageInfo by value (optional) | `ImageInfo::CachedRebase` | 12 | 0.1 |

Items 1-1b are the bulk of the CRT time (691 samples under CopySparseMemory, of which 628 are the
memcpy itself), and they are the data the GPU needs. They are not avoidable copies in the sense of copying
unchanged state: the bytes are dirty guest pages that must reach the device.

### What the top item actually costs

The destination of item 1 is `staging.mapped`. In `vk_staging_buffer_pool.cpp:109` the staging
is `HostUncached` (write-combined) unless `SOTC_STAGING_CACHED=1`. The code comment there
records 2.6-6 GB/s for memcpy into write-combined memory in area-load frames (92% of draw
thread samples in "sync copy" were that memcpy). So the 628 samples are dominated by WC write
bandwidth, not by the guest read. The switch already exists (documented in
`docs/sotc/README-SotC.md:160`, off by default). Proposed action: measure the 20 s profile with
`SOTC_STAGING_CACHED=1` before writing anything new. No diff is proposed for item 1; a
different copy path would not remove the WC write.

### Items that are avoidable (candidates)

- Item 4, flattened user data copied twice per draw op. Path: the front thread copies
  `rt.flattened_ud_buf` into `stage.flat` (`liverpool.cpp:1004`), then the draw thread copies
  `stage.flat` into `runtime[1].flattened_ud_buf` (`liverpool.cpp:1132`). The second copy can be
  a move. It is safe only when no prep worker reads `stage.flat` concurrently (prep workers run
  `setup_stages` on the same op, `liverpool.cpp:1220`), so the move is gated on
  `prep_workers == 0`. Single execution is guaranteed: an op that ran ahead has `xp_done` set and
  is not executed again (`liverpool.cpp:646-647, 1926-1927`). The swap leaves the ring slot's
  `stage.flat` with stale content, which the front copy (`:1004`) overwrites on reuse.
  Expected gain: the copy of the flat buffer (up to 131 samples, about 1.1%); unmeasured.

- Item 2, `ReplaceChangedPages` memcmp (131 samples). Compares every page of the destination with
  the source each frame, even when the source has not changed. Avoiding it needs a source
  generation from the memory tracker. That is a larger change than a diff here; not written.

- Item 6, `BuildTable` (95 samples). It runs for tables whose pattern period is over 64 KiB
  (the page-replace path is skipped by `period <= 64_KB`, `vk_shader_hle.cpp:~470`). The pattern
  path would copy `period + 4 KiB` instead of `count * 4`. Not written: the share of periodic
  tables above 64 KiB is unmeasured and `!in_run` may account for most of the samples.

## Diff: item 4 (SOTC_UD_SWAP, off by default)

File: `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, in `Liverpool::ExecuteBackOp`, the stage loop.

```diff
--- a/gpu/shadps4/video_core/amdgpu/liverpool.cpp
+++ b/gpu/shadps4/video_core/amdgpu/liverpool.cpp
@@ void Liverpool::ExecuteBackOp(BackOp& op) {
+    // sotc: SOTC_UD_SWAP=1 (off by default): with no prep workers the draw thread is the only
+    // reader of stage.flat, and an op runs once (xp_done), so the flattened user data is moved
+    // into the runtime slot instead of copied (the front thread's copy is the only other one).
+    static const bool ud_swap = [] {
+        const char* v = std::getenv("SOTC_UD_SWAP");
+        return v && v[0] == '1';
+    }();
@@ in the loop `for (u32 i = 0; i < op.num_stages; ++i) { BackStage& stage = op.stages[i]; ... }`
         Shader::InfoRuntime& rt = stage.info->runtime[1];
         rt.user_data = stage.num_user_data == ~0u
                            ? std::span<const u32>{}
                            : std::span<const u32>{stage.user_data.data(), stage.num_user_data};
-        rt.flattened_ud_buf.assign(stage.flat.begin(), stage.flat.end());
+        if (ud_swap && prep_workers == 0) {
+            rt.flattened_ud_buf.swap(stage.flat);
+        } else {
+            rt.flattened_ud_buf.assign(stage.flat.begin(), stage.flat.end());
+        }
         rt.pgm_base = stage.pgm_base;
```

Checks needed before enabling: `prep_workers` is the Liverpool member set at `liverpool.cpp:284`;
add `<cstdlib>` if `std::getenv` is not already in scope; verify that nothing reads
`op.stages[i].flat` after the swap (grep showed only `:1004`, `:1111`, `:1132`, `:1220`).

## Not proposed

- Item 8 (push descriptor copies): the recorder copies descriptor arrays because the command is
  replayed on another thread. Removing that copy needs a different recorder design.
- Item 9 (stream buffer copy): this is the upload itself.
- `IntervalList` shifting: an algorithmic cost of the container.

## Next measurement

1. Profile again with `SOTC_STAGING_CACHED=1` to size the WC penalty on items 1-1b.
2. Profile with `SOTC_UD_SWAP=1` (after adding the diff) and compare the `ExecuteBackOp` memmove
   leaf (the 0x14038f555 call site, inclusive count) against the 131-sample baseline.
