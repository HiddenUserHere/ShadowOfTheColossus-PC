# Direct draw: making the buffer and texture caches safe for concurrent resolve

Status: design, plus the buffer groundwork listed in section 6. Branch `direct/vulkan` (Release 0.76.2).
Parent document: `docs/engine/direct-draw-design.md` (sections 1, 4, 5). Profile numbers are the inclusive
shares from that document (attract demo, draw thread 25-26 ms per frame). Figures marked "est." are
estimates from those shares, not measurements. Line numbers are from before the groundwork change unless
a name is given.

## 1. What the bind path calls per draw

Classes: **(a)** pure read of cache state; **(b)** read that writes bookkeeping (LRU/use epochs, stats,
memo tables, per-draw flags); **(c)** creates, uploads, evicts, binds memory, or changes layouts. Class (c)
must stay on the single apply thread.

### 1.1 Buffers (per stage, `Rasterizer::ResolveBuffers` and `ApplyBuffers`, vk_rasterizer.cpp:3579-3823)

| Call | Location | Class | Notes |
|---|---|---|---|
| `memory->ClampRangeSize` | ResolveBuffers 3618 | a | guest VMA query, not cache state |
| `BufferCache::LookupArena` (new) | ResolveBuffers 3647 | a | pure, versioned by `arena_map_version` |
| `IsRegionGpuModified` (stream check) | `ObtainBuffer` 1293 | a | plain bit reads, `region_manager.h:324`; not reached at the default threshold (0) |
| `GetArena` hit path | `ObtainBuffer` | a | returns `address_space[page]`, no side effects |
| `GetArena` miss (create, migrate) | `buffer_cache.cpp:1763` | c | writes `address_space`, `arenas` |
| `EnsureResident` | `buffer_cache.cpp:1850` | b + c | b: stamps `block_last_use`, `resident_cache`; c: sparse binds, `ForcePendingReleases` |
| `SynchronizeMemory`, clean ranges | `ForEachUploadRange`, unlocked look `region_manager.h:236-250` | a | atomic loads of the CPU bits; validated by `CpuGenSum` (`region_manager.h:219`) |
| `SynchronizeMemory`, dirty ranges | same, locked scan, `CopySparseMemory`, `runtime.CopyBuffer` | c | clears CPU bits, may raise grace; `RecordCurrentWrite` is b |
| `SynchronizeMemoryFromImage` (texel, read) | 2031 | c | re-tiles an aliasing image |
| `NoteHostWrite`, `RecordCurrentWrite` (written bindings) | `ObtainBuffer` 1309-1313 | b | stats, hot-write tick |
| `AddGpuModified` (written bindings) | 1312, 1318 | c | `gpu_modified_ranges` under unique `gpu_modified_mutex` |
| `texture_cache.InvalidateMemoryFromGPU` (written) | ApplyBuffers 3806 | c | can invalidate images |
| `runtime.IsBufferAccessed` | ApplyBuffers 3810 | b | barrier bookkeeping; confirm in `vk_runtime` before workers |

Fault path: `fault_manager.cpp:180` calls `ObtainBuffer` from a `scheduler.DeferOperation` (line 168), so it
runs on the thread that drains the scheduler, not on a guest thread. That is the only caller outside the
bind path, and it must stay on the apply side.

### 1.2 Textures (`Rasterizer::BindTextures`, vk_rasterizer.cpp:4225; `BindResolvedImage` 3913)

| Call | Location | Class | Notes |
|---|---|---|---|
| `texture_cache.BatchLock()` | BindTextures 4226 | lock | exclusive, see 2.2 |
| `FindImage`, memo hit | texture_cache.cpp:822, `MemoLookup` 2525 | a for the probe; b for `TouchImage`, `tick_accessed_last`, `memo_stale` | the common case |
| `FindImage`, miss | 851-946 | c | overlap resolution, `FreeImage`, `TakeStashedImage`, slot insert, `RegisterImage`, `MipTailCreated`, `FreeRebased` |
| `DecideRebase` | 2564 | b | swaps `desc.info` and `other_info` |
| `GetAssociatedDepth` | 4267, 4388 | a (confirm) | |
| `GetImage` | 4266, 4385 | b | LRU touch |
| `image->binding.is_bound = 1` | 4271, 4399 | b | per-draw flag, read again by the feedback check (4874) |
| `IsMeta` | 4326 | a | read only |
| `FindTexture` to `Image::FindView` | 3920; image.cpp:298 | b (hit: `backing->last_view`), c (miss: view insert, `SetBackingSamples`) | |
| `BindResolvedImage` `Transit` | 3929-3957, 4432-4463 | c | layout state is single-owner |
| `TexBindFastCheck` / `Apply` | 3977-4007 | b | LRU touch, usage flags |
| `TexBindMemoFresh`, `ImageSetGeneration`, `FindGeneration` | 4017; texture_cache.h:382-391 | a | plain reads today; must become atomic |

## 2. Current concurrency model

### 2.1 Buffers

There is no cache-wide lock on the bind path. Protection is per structure:
- `gpu_modified_mutex` (`std::shared_mutex`, buffer_cache.h:308): shared for readers such as `ReadClean` and
  `IsRangeGpuWrittenLocked`, unique for writers such as `AddGpuModified` and `FinishAsyncReadback`.
- `RegionManager::mutex` (`std::mutex`, region_manager.h:67, 498): held by the locked scans and by
  `ChangeRegionState`. The CPU-bit scan has an unlocked fast path, so most draws never touch it.
- CPU bits and `cpu_word_gen` (region_manager.h:364): atomics, bumped after a CPU bit rises. This is
  the existing validation primitive: `SnapshotCpuGen` and `CpuGenUnchanged` (region_manager.h:176-197).
- `address_space`, `arenas`, `resident_version`, `block_last_use`, `resident_cache`: plain, written only
  by the thread that runs `ObtainBuffer` (today, the scheduler thread). `arenas` is a `std::deque` that is
  never erased, so arena pointers are stable.
- `IsRegionModified<GPU>` reads the bit arrays with plain loads (region_manager.h:324), which is a data race
  if a worker runs it. The CPU path uses `__atomic_load_n` (region_manager.h:242).

### 2.2 Textures

`TextureCache::ReentrantMutex` (texture_cache.h:69-238) is exclusive. It is biased to the first thread that
takes a batch hold (the command processor). That thread pays only a plain store and load. Any other thread
takes the spinlock `word`, sets `slow_wants`, and runs `AsymmetricBarrier` (a `FlushProcessWriteBuffers`
on Windows) before it waits. So every off-thread lock acquisition costs a barrier. Workers therefore cannot
take this lock per lookup: they would serialize on it and pay a barrier each time.

The rest of the cache state:
- `find_memos`, `image_set_generation`, `addr_gens`, `surface_metas`, `slot_images`: plain, under `mutex`.
- `meta_epoch`, `sampler_epoch`, `gpu_modified_epoch`, `page_images`: atomics, readable without the lock.
- `samplers_mutex`, `download_images_mutex`: `std::mutex`, not on the bind path.
- `Image::FindView` writes `backing->last_view` on every hit, so it is a write even when it looks like a read.

## 3. Target model

### 3.1 Buffers

1. **Resolve (workers, class a):** `LookupArena` plus the clean check. The clean check reads CPU bits
   atomically and records `CpuGenSum` for the range. No lock is taken.
2. **Validation (apply thread, one load each):** a result is used only if `ArenaLookup::Valid` holds (same
   address and size, and `arena_map_version` unchanged) and `CpuGenUnchanged` holds for each clean range.
   Both are existing or new single loads.
3. **Bookkeeping (class b):** each worker keeps a log of `EnsureResident` stamps, `RecordCurrentWrite`
   ticks and `is_bound` marks. The apply thread replays it in sequence order.
4. **Apply (class c, apply thread):** `EnsureResident` binds, dirty `SynchronizeMemory`, and `AddGpuModified`.
   These stay in `ApplyBuffers`, which already runs in order.
5. **Fallback:** if validation fails, `ApplyBuffers` runs the current serial path for that one binding
   (`ObtainBuffer` with no hint). The result of the worker is discarded.

Prerequisite: `resident_version` and `address_space` become atomic (or are read under a seqlock) before
workers read them. The current `ObtainBuffer` hint works without that, because only the apply thread runs.

### 3.2 Textures

1. **Index snapshot (RCU-style):** the address-to-image lookup is published as an immutable per-1 MiB-page
   vector (the page table already keys by 1 MiB pages, `page_images`). Register and unregister copy the one
   page vector, swap the pointer, and retire the old vector. Retired vectors are freed when every worker has
   passed the sequence number of the retire (epoch reclamation). Workers load the pointer with acquire.
2. **Per-image version:** `Image` gets `std::atomic<u64> version`. It is bumped on register, unregister,
   `info` change, view insert, `Transit` and `is_bound`. A worker records `(image_id, image_uid, version)`.
   The apply thread accepts the result if all three still match.
3. **Memo:** the find memo is read by workers through the same snapshot. Writes go to the apply thread, which
   stores the memo after validation, using the existing `MemoGeneration` (per-address) rule.
4. **Misses are not resolved on workers:** a worker that finds no perfect match returns "defer". The apply
   thread runs the existing `FindImage` (class c), so no worker creates or overlaps an image.
5. **Bookkeeping (class b):** `TouchImage`, `tick_accessed_last`, `binding.is_bound`, `last_view` and stats
   go into the worker log and are applied in order by the apply thread.
6. **Fallback:** a failed version check re-runs that binding inline (the legacy `BindTextures` body).

### 3.3 Locks

- Workers take no `ReentrantMutex` in steady state. The lock stays for class (c) work on the apply thread.
- The snapshot needs no lock to read. Writers (register and unregister, on the apply thread) take `mutex`,
  so the existing invariants still hold.

## 4. Expected gain per item (est. from the shares in direct-draw-design.md section 4)

| Item (draw thread share) | Moves to | Est. draw-thread gain | Confidence |
|---|---|---|---|
| `BindResources` (41.8%, about 10.9 ms) | workers (a), apply (c) | the largest item, sum of the rows below | high in direction |
| `BindTextures` (16.2%, 4.1 ms) | workers for memo-hit lookups and view hits, apply for misses and transitions | 1.5-2.5 ms | medium: hit rate not measured |
| `TextureCache::FindImage` (6.2%, 1.6 ms) | memo-hit probe on workers | about 1.0 ms if hits are about 65% | low until hit rate is counted |
| `ImageView` `FindView`, push descriptors (est. 4 ms inside BindResources) | worker-local `last_view` hint and descriptor writes | 1-2 ms | medium |
| `SynchronizeMemory` (9.5%, 2.4 ms) | clean check on workers; dirty uploads to the sweep and upload threads | up to 2 ms if clean ranges dominate | medium: clean/dirty split not measured |
| `ObtainBuffer` (8.9%, 2.3 ms) | address lookup only (done) | 0.2-0.3 ms; most of the cost is `EnsureResident` and `SynchronizeMemory` | high that the lookup is small |

Caution: these shares are inclusive, so rows overlap, and the sum is not additive. Before step 2 of the
migration plan, count the clean/dirty split of `SynchronizeMemory`, the memo-hit rate of `FindImage`, and
the `last_view` hit rate. Use `SplitTimer` entries (`SotcStats::SplitBufObtain`, `SplitBindTextures`) to
measure them. Do not commit to a gain figure before those counts exist.

## 5. Validation cost and failure

- Buffers: one acquire load of `arena_map_version`, plus one `CpuGenSum` recheck per clean range. Both
  are cheap compared with the current `SynchronizeMemory` call.
- Textures: one load of the image `version`, plus the snapshot pointer compare.
- On failure, the binding is re-resolved inline on the apply thread. The result is then the same as the
  legacy path, because the legacy path is the reference. A failure is counted (new `SotcStats` counter)
  so that the rate can be checked against the profile.

## 6. Groundwork in this change (buffers only, behaviour unchanged)

- `BufferCache::ArenaLookup` and `LookupArena` (buffer_cache.h, buffer_cache.cpp:1282). Pure read of
  `address_space`. Same hit rule as `GetArena`'s single-arena branch, which has no side effects.
- `BufferCache::arena_map_version` (atomic, buffer_cache.h:363), bumped with release after every write to
  `address_space` in `GetArena` (two places).
- `ObtainBuffer` takes an optional `hint`. When `hint.Valid(addr, size, ArenaMapVersion())` holds, its
  arena is used instead of `GetArena`. For the same range and no intervening arena change, `GetArena` returns
  that same pointer, so the result is identical.
- `ResolvedBuffer::arena` (vk_rasterizer.h:42), filled by `ResolveBuffers` (vk_rasterizer.cpp:3645-3648),
  and passed as the hint by `ApplyBuffers` (3798).

Cost today: one extra `LookupArena` per buffer binding (a few loads). A hint is discarded when an
earlier binding of the same stage created or migrated an arena; the full path then runs as before.

Not changed: textures (design only, section 3.2); `resident_version` and `address_space` stay plain.

## 7. Risks and open items

- **Not built or run.** The edits were made without compiling, as the brief asked. The main session must
  build and run the attract frame-hash check.
- `address_space` is a plain array. Any future worker read needs atomic entries or a seqlock. Changing its
  type touches about 20 sites, so it is deferred.
- `Image` and `ImageView` pool stability (`slot_images`, `slot_image_views`) is assumed by `FindView` and
  `GetImage` and must be confirmed before a worker holds a reference across a register.
- `runtime.IsBufferAccessed` and `GetAssociatedDepth` are classified (b) and (a) without reading their
  bodies. Confirm both before the worker split.
- Lock acquisition from workers costs an `AsymmetricBarrier` (Windows: `FlushProcessWriteBuffers`). Keep
  the worker path free of `ReentrantMutex` calls, and measure the barrier cost if any slips in.
- Guest write faults (`MarkWriteAhead`, `InvalidateMemory`) can run on guest threads. They use `page_images`
  and the region locks, not the texture lock. Their interaction with the snapshot retire list must be designed
  before step 3 of the migration plan.
- Fallback rate: if validation fails often, the speed-up disappears. The counter in section 5 is the check.
