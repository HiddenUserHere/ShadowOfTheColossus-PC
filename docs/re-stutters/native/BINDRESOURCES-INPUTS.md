# Rasterizer::BindResources: input and side-effect inventory

Scope: `Rasterizer::BindResources` (vk_rasterizer.cpp:3706) and the functions it calls on the
full-bind path, the cache-hit path and the fast-bind path: BindBuffers (6514), BindTextures (7690),
BindTexturesFull (7881), BindHeapImages (7625), BindSamplers (8503), TableMemo::Resolve
(sotc_arch/table_memo.cpp), the packet and heap-block fill, PacketAddrReplay (4388), ResetBindings
(6168) and Pipeline::BindResources (vk_pipeline_common.cpp:486). Line numbers are those of this tree
(commit 80225cd plus the synced main-checkout files, plus the SOTC_FPS counters added for Task A).

Method: the function bodies were read at their decision points and searched for every register,
sharp, flat-buffer word, guest-memory read, cache generation and epoch they consult, and for every
write they make. Not every branch of the 1,100-line BindBuffers or the 1,200-line BindTextures was
read line by line; rows marked "grep" were located by search and not traced in full. Treat the
table as a complete map of the input kinds, not as a line-exact proof for every branch.

## Keys and classes

Class codes used in the tables:

| Code | Meaning |
|------|---------|
| 1 | In key D (the descriptor key), or in the bind cache key (`BindResourceHash` + `bc_base` + `BindCacheStageIdentity`) |
| 2 | Per-op data: read or refreshed on every replay (hit, fast or wide), not part of any key |
| 3 | Cache or global state: a change needs an invalidation event (flush, generation, invalidate-by-range) |
| 4 | Makes the op unrecordable: the bind is refused, dropped, or uncacheable |

Two keys are in play and they differ:

- **Key D, the OpSetStats measurement key**: `DescriptorHash(pipeline)` (vk_rasterizer.cpp:1973),
  combined with the draw target (`key_d`, line 2102). It hashes each stage's T#, S# and V# words
  and zeroes the base address of shader-addressed V#s. It is a measurement of repetition, not a
  cache key.
- **The frame bind cache key**: `BindResourceHash` (3309) hashes the pipeline pointer, the stage
  identity (`BindCacheStageIdentity`, 3285), and the full T#/S#/V# words including the base of
  shader-addressed V#s. The cache then mixes in `ta_bc_base` (4073) and the target key
  (`BindCacheFrameKey`, 4110-4112).

Consequence: the cache key is finer than key D in two ways: it keeps the base of shader-addressed
V#s, and it adds the border colour base and the target identity. A bind that repeats key D can
still miss the frame cache when a shader-addressed base moves, or when a target changes. This is a
hypothesis for the gap between the 97% repeat under key D and the few consulted hits seen in the
measurements; it has not been measured.

## A. Inputs

| # | Input | Read at (file:line) | Class | Notes |
|---|-------|---------------------|-------|-------|
| 1 | Pipeline identity: pointer, `IsCompute()`, `GetProfile()` (heap_images, shader-addr profile), `UsesStageSets()` | 3309 (BindResourceHash), 1973 (DescriptorHash), 3706-3708 (SetBindProfile) | 1 | Pointer is in both keys. The profile selects the shader-addr and heap paths. |
| 2 | Stage identity: `CacheHash()`, buffer/image/sampler counts, sw_stage, descriptor layout kinds | 3285 (BindCacheStageIdentity), 3309 | 1 | Keeps entries valid across pipeline erase and address reuse. |
| 3 | Stage list `GetStages()`; a stage whose `UserData()` is null | BindResources stage loop; no-user-data check at 3953 (bind_post_nodata) | 1, 4 | A null user-data stage drops the op after the cache lookup gate. |
| 4 | Buffer V# words (`desc.GetSharp(stage)`), including base address and stride | 6761 (BindBuffers); 3337 (BindResourceHash); 1981-1990 (DescriptorHash) | 1 | In key D with the shader-addressed base zeroed; in the cache key with the base kept. |
| 5 | Image T# words (`image.GetSharp`) | 7377, 7919 (BindTextures); 3342 (BindResourceHash) | 1 | In both keys. |
| 6 | Sampler S# words (`sampler.GetSharp`) | 8509 (BindSamplers); 3346 (BindResourceHash) | 1 | In both keys. |
| 7 | Flat buffer words of each stage (`FlatBuf()`, `FlatForBind`) | 6441, 6649-6650 (BindBuffers) | 2 | Copied to the stream ring on every bind. The rest of the flat buffer (user-data constants, SRT values) is not in BindResourceHash (3311-3318) and is re-copied on a hit. |
| 8 | Flat buffer patched by live settings or TAA resolve | 6651-6656 (BindBuffers) | 4 | Makes the bind uncacheable. |
| 9 | User data registers (inside the flat buffer) | FastBindHash 3255-3280 (hashes `UserData()` and `FlatBuf()`) | 1 (fast key), 2 | The fast-bind key includes them; the cache key does not. |
| 10 | Push data (`MakeUserData(regs)`) | 146 (MakeUserData); FastBindHash 3255; bind_input_push refresh (cache hit) | 1 (fast key), 2 | The fast hash includes it; a cache hit refreshes it. |
| 11 | Viewport words in push data | bind_input_push refresh on a hit | 2 | Refreshed on every hit; excluded from BindResourceHash. |
| 12 | Colour and depth buffer addresses (`color_buffers[].Address()`, `depth_buffer.DepthAddress()`) | FastBindHash 3255-3280 | 1 (fast key), 3 | Fast key only; the cache uses the target identity (13). |
| 13 | Render target identity: `cb_descs`, `db_desc`, image uid, mrt_mask | BindCacheSyncFrame (~5072-5110, grep) | 3 | Target key in the cache key. A draw that writes its targets is ineligible for the cache and invalidates them (BindCacheInvalidateImage, 4658). |
| 14 | `ta_bc_base` (border colour base) | 4073 (bc_base into the cache key); 7365 (TableMemo key); 8527-8540 (GetSampler) | 1, 2 | In the cache key; read per bind for custom-border samplers. |
| 15 | `clipper_control.user_clip_plane_enable` | BindCacheRefusal (BgClip, 4424+); 6678 (BindBuffers) | 4 | Refused by the cache and by FastBindStatic. |
| 16 | `clip_user_data[]` plane words | 6684 (BindBuffers) | 2 | Read per bind into the clip buffer. |
| 17 | `depth_control.stencil_enable`, depth image `is_storage` | 7267 (BindTextures) | 2, 3 | Not in key D nor in BindResourceHash; covered only through the target key. Needs confirmation. |
| 18 | `attachment_feedback_loop` | 3203-3205 (FastBindEligible) | 4 | Refuses the fast bind; the cache takes an uncacheable mark at 8233. |
| 19 | Shader-addressed V# (ShaderAddrBuffer, profile) | 6624-6636 (BindBuffers) | 1, 2, 3, 4 | Read-only ones are replayed by a hit (PacketAddrReplay, 4388) and checked for guest-write hazards (GuestWriteOverlap). Written ones are uncacheable (6864-6866). |
| 20 | Buffer range from V# (vbase, size) and its mapping (`memory->IsMappedAddress`) | 6761-6790 (BindBuffers) | 1, 3 | Range comes from the sharp (key); the mapping can change (bind_cache_map_dep_rec, 6881). |
| 21 | Guest view of a range (`LookupGuestView`, `direct_answer`) | 6553-6589 (BindBuffers) | 2, 3 | Per-op view; depends on guest mapping state. |
| 22 | Written buffer flag (`desc.is_written`) and special buffers | BindCacheRefusal (BgWrittenBuf, 4424+); 7109-7180 (BindBuffers) | 4 | Written buffer ranges make the op ineligible; the cache flushes or invalidates by range (BindCacheInvalidate, 4608). |
| 23 | Written image flag (`image.is_written`) | BindCacheRefusal (BgWrittenImg, 4424+) | 4 | Ineligible. |
| 24 | Formatted (texel) buffers (`desc.is_formatted`) | 7127-7129 (BindBuffers) | 2, 3 | Re-tiled from an aliasing image by every bind (comment near 6944); the cache records texel repairs. |
| 25 | Shared memory (LDS) descriptor and `cs_regs` size | 6703-6710 (BindBuffers) | 2, 4 | Compute only; refused by the cache. |
| 26 | GDS buffer | 6640-6646 (BindBuffers) | 4 | Refused when written (BindCacheRefusal, GdsBuffer case). |
| 27 | Stage DMA pointers (`uses_dma`), `sharp_tables`, `fmasks`, `has_storage_images` | BindResourceHash 3324-3326; BindCacheRefusal (4424+) | 4 | Refused. `uses_dma` also triggers `SynchronizeDmaBuffers` at the end of BindResources. |
| 28 | Image array size, mip fallback, mip tail mode (`MipTailMode`), `rebase_mip`, `lod_hw_cnt_en` | 7878, 8158 (BindTextures); 3329 (array_size, BindResourceHash) | 3, 4 | Array images are not hashed (BindResourceHash returns false). Mip fallbacks are refused by the table memo. |
| 29 | LOD counter banks (`counter_bank_id`) | 7953, 7975 (BindTextures) | 2 | Recorded and repeated on a hit (bind_cache_lod_rec). |
| 30 | Composite and UI linear probes (`composite_probe`, `UiLinearAsset`, Dlssg) | 7347-7356, 7594-7598, 8445-8451 | 4 | Uncacheable marks. |
| 31 | Block limits: `MaxBlockImages`, `MaxBlockSamplers`, empty stage | 7351-7355 (BindTextures) | 4 | Bind takes the classic path, not the table memo. |
| 32 | Feedback and storage image checks in the texture pass | 8233, 8247, 8260, 8350 (BindTexturesFull) | 4 | Uncacheable marks. |
| 33 | BDA table stage (`bda_stage`) | 6606-6608 (BindBuffers) | 4 | "The table is per draw": uncacheable. |
| 34 | Kernel TAA resolve hash | 6653 (BindBuffers) | 4 | Uncacheable. |
| 35 | Compute image copy, clear, meta clear, image clear predicates | 3893-3935 (BindResources; IsComputeImageCopy at 3893, post-drop counter 3934) | 4 | Replace the dispatch; the op is dropped after the eligibility gate (counted as a post-gate drop). |
| 36 | Two heap stages of one kind; index table device address 0 | 3800, 3824, 3846 (BindResources, heap counters) | 4 | Op dropped before the cache lookup (counted as heap dropped). |
| 37 | Heap index table contents (T#/S# resolution result) | TableMemo::Resolve 3815, 3837; sotc_arch/table_memo.cpp:28-35 (validity list) | 2, 3 | Per-op address (2). Entries are valid while the T#/S# words, border colour base, image addr generation, bind generation, gpu_modified_epoch and heap slot holds are unchanged (3). |
| 38 | Index table address per op (`draw_packet.index_table_address`) | 3809, 3827 (BindResources) | 2 | Changes per op; not in any key. |
| 39 | Flat-buffer ring address (`draw_packet.flatbuf_address`, `flatbuf_fragment_address`) | 6659-6665 (BindBuffers) | 2 | Per op. |
| 40 | Mirror and address-table windows (`draw_packet.mirror_*`) | 3762-3777 (BindResources, packet_op) | 3 | Changes when the mirror or address table is remapped. |
| 41 | Shader address verify address (`VerifyAddress()`) | 3809 | 2 | Debug path. |
| 42 | Prepared inputs from the prep thread (`cache_eligible`, `res_ok`, `res_hash`, `fast_hash`, `push_data`, `table_keys`, `resolved`) | BindResources argument `prepared`; 4150-4153 (resolved table generation: `runtime_memory_generation`) | 1, 2, 3 | Hashes are the key inputs computed early; the resolved table is valid only under the same runtime memory generation. |
| 43 | Bindless generation (`BindlessGeneration()`) | 3736 (bind_bindless_gen); 7421, 7525 (BindTextures memo) | 3 | Any bindless change invalidates stored binds. |
| 44 | Bindless heap slots (`bindless_images->Holds/Use/Verify`) | 7574-7578 (BindTextures); 8393-8395 (BindTexturesFull) | 2, 3 | A slot may be evicted by a later acquire; the fast bind does not mark slots used. |
| 45 | Texture cache bind generation (`texture_cache.BindGeneration()`) | 7427, 7894, 8462, 8814 | 3 | Stored binds check it on each hit. |
| 46 | Texture cache sampler generation (`SamplerGeneration()`) | BindCacheSyncFrame (~5072-5110, grep) | 3 | A destroyed sampler bumps it and triggers a resources flush. |
| 47 | Image address generation (`ImageAddrGeneration(addr)`) | 7444 (BindTextures memo) | 3 | Per memo entry. |
| 48 | Image lookups and state: `FindImage`, `TryGetImage`, dirty and rebind flags, `FindView` | 7731, 7958, 8129, 8211, 8462-8471 | 3 | Each hit re-checks that the image is registered, not dirty, and needs no rebind. |
| 49 | Buffer cache bind version (`buffer_cache.BindVersion()`) | 6978-6984 (memo), 7032, 7427 (grep) | 3 | Changes on unmap or eviction. |
| 50 | Stream ring generation (`buffer_cache.StreamGeneration()`) | 6667-6672 (ring copy record); fast-state check (4004) | 3 | Changes when the ring wraps; stored stream offsets are invalid after. |
| 51 | BDA table offset in the stream ring (`bda_table.Lookup/Store`) | 6719-6723 (BindBuffers) | 2, 3 | Keyed by (pgm_hash, StreamGeneration). |
| 52 | Guest-memory contents read by the CPU: dirty checks, LDS table compare | 6713-6730 (BindBuffers, grep) | 2, 3 | Compares words uploaded earlier in the stream buffer; contents are data, not a key. |
| 53 | Global memory epoch (`SotcStats::mem_epoch`) | 4150-4153 (bind_rec); fast-state compare (4004); BindCacheSync | 3 | Bumped by any guest write (NoteHostWrite). |
| 54 | GPU-modified epoch (`VideoCore::gpu_modified_epoch`) | 4153 (bind_rec_gpu_epoch); 7428 (BindTextures memo) | 3 | Mip-tail candidates need it unchanged. |
| 55 | Map epoch (`MapEpoch()`) and write stamps (`write_stamps.Seq()`) | 4151-4152 (bind_rec) | 3 | Recorded at bind start; used by the commit. |
| 56 | Submit tick (`scheduler.CurrentTick()`) | BindCacheSync and BindCacheSyncFrame (`now.tick`); fast_bind.tick (4034) | 3 | Entries are valid within a tick unless the xtick repair path replays them. |
| 57 | Cache state: `bind_cache_state`, `bind_cache_flush_gen`, `bind_cache_live`, frame entries | 4466 (BindCacheFlush), 4490 (BindCacheSync), 5072 (BindCacheSyncFrame) | 3 | Generation counter flush for the frame cache. |
| 58 | Invalidation events from other ops: `BindCacheInvalidate`, `BindCacheInvalidateImage`, `BindCacheNoteWrite` | 4608, 4658, 4670 | 3 | These are the invalidation events a cached bind relies on. |
| 59 | Write epoch (`bind_cache_write_epoch`) | BindCacheSync (~4490-4560, grep) and BindCacheSyncFrame | 3 | Written resources of an earlier op. |
| 60 | Fast-bind record (`fast_bind.*`: pipeline, generations, hash, tick, uses_dma, addr) | CommitFastBind 3354; fast-state check (4004) | 3 | A second cache: its generations must be checked the same way. |
| 61 | Constants for the run (env switches, `TsharpSlotCache::Mode()`, BDA and fault buffers, lod feedback buffer, composite probe, GDS and stream buffer handles) | 7904 (grep), 6696-6701, 7347 | (none) | Run constant; not counted. |

## B. Side effects (outputs, not inputs)

| # | Effect | Made at (file:line) | Notes |
|---|--------|---------------------|-------|
| S1 | Descriptor writes: `set_writes`, `buffer_infos`, `image_infos` | BindBuffers 7193-7199; BindTextures 7584 | Consumed by Pipeline::BindResources. A shader-addressed slot keeps zeroed entries (comment at 4268-4272). |
| S2 | Push constants and descriptor sets or heap rings | Pipeline::BindResources (vk_pipeline_common.cpp:486-520, and the heap path) | Push constant cache is per command buffer (dynamic.push_constant_cache). |
| S3 | Buffer residency: `ObtainBuffer` (uploads, placement, eviction) | 7020 (BindBuffers) | Answer is used for the bound range and marks. |
| S4 | Touch of a shader-addressed range | 6810 (AddrTable::Touch or Mirror::Touch); PacketAddrReplay 4388 | Replays repeat it on a hit. |
| S5 | Host write notes: mem_epoch bump, write marks | 6856, 7171 (NoteHostWrite) | Global side effect: invalidates memory-epoch dependent cache state. |
| S6 | Guest write tracking: `sotc_op_writes`, `sotc_addressed_writes`, `MergeOpWrites` | 6823, 6844, 7117 (BindBuffers); MergeOpWrites 4365 | For the shader-address ordering of later ops. |
| S7 | Bound buffer record and barrier accesses | `bound_buffers` 7109; ResetBindings 6187 (`runtime.AccessBuffer`) | Barrier per buffer (static barrier mode skips it). |
| S8 | Barrier decisions: `buffer_barrier_need`, `packet_barrier`, `DecideOpBarrier` | packet_barrier lambda at 3752; DecideOpBarrier 4375 | Packet hits also replay the barrier rule. |
| S9 | Image access stamps: `tick_prev_accessed`, `tick_accessed_last` | 7789-7791 (replay of recorded lookups) | Restored after a replay. |
| S10 | Image binding state reset | ResetBindings 6171 (`binding = {}`) | Per op. |
| S11 | Bindless slot use | 7576-7578, 8393-8395 | Affects slot eviction. |
| S12 | Sampler creation or lookup | `texture_cache.GetSampler` 8539-8540 | May create a Vulkan sampler; destruction bumps SamplerGeneration. |
| S13 | DMA synchronization | `buffer_cache.SynchronizeDmaBuffers()` (4022, 4049, 4289) | Runs when `uses_dma`. |
| S14 | Cache recording (`bind_cache_*_rec` vectors) | 6667-6672, 6830-6835, 7125-7136, 7532-7534, 8541-8543; commit in CommitBindCacheFrame (5580) | Recording only while `bind_cache_recording`. |
| S15 | Stream ring copies of flat buffers and LDS tables | 6657 (Copy), 6692, 6704-6732 (BindBuffers) | Each full bind writes them. |
| S16 | Fast-bind record and packet state | CommitFastBind 3354; `draw_packet` fields 3762-3777 | |
| S17 | Cache flushes and invalidation | BindCacheFlush 4466; BindCacheInvalidate 4608; BindCacheInvalidateImage 4658 | The unsafe flush sites are counted by SOTC_FPS (Task A). |
| S18 | Uncacheable and recording marks | 4591 (BindCacheNoteImage), 6608, 6655, 6867, 7161, 7598, 8233-8350, 8451 | Set `bind_cache_uncacheable` for this bind. |

## C. Summary

Counts are over the 60 rows of table A that carry a class (row 61 is the run constant and carries
none). A row can carry several classes.

| Class | Inputs (rows of table A) | Count |
|-------|--------------------------|-------|
| 1 (in a key) | 1, 2, 3, 4, 5, 6, 9, 10, 12, 14, 19, 20, 42 | 13 |
| 2 (per-op, refreshed on replay) | 7, 9, 10, 11, 14, 16, 17, 19, 21, 24, 25, 29, 37, 38, 39, 41, 42, 44, 51, 52 | 20 |
| 3 (needs an invalidation event) | 12, 13, 17, 19, 20, 21, 24, 28, 37, 40, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60 | 29 |
| 4 (unrecordable) | 3, 8, 15, 18, 19, 22, 23, 25, 26, 27, 28, 30, 31, 32, 33, 34, 35, 36 | 18 |
| none (run constant) | 61 | 1 |

Unrecordable (class 4) inputs, the rows that stop an op from being recorded or make the bind refuse
the cache: 8 (flat buffer patched), 15 (clip plane), 18 (feedback loop), 19 (written shader-addressed
range), 22 (written buffer), 23 (written image), 25 (shared memory), 26 (GDS), 27 (DMA, sharp tables,
fmasks, storage images), 28 (mip fallbacks, array images), 30 (composite and UI probes), 31 (block
limits), 32 (feedback in the texture pass), 33 (BDA table), 34 (TAA resolve), 35 (copy or clear
dispatch), 36 (heap drops). Row 3 (a null user-data stage) is also class 4.

Observations for the next step:

- The frame cache key (BindResourceHash, 3309) keeps the base of shader-addressed V#s; key D
  zeroes it. Any repeated op whose shader-addressed base moves is a repeat in D but a miss in the
  cache.
- Flat buffer words of user data are per op (class 2) and are re-copied on every hit. A change in
  the constants the shader reads never reaches the key. This is safe only if the shader reads
  constants from the copy and not from the descriptors; that assumption still has to be verified.
- Global epochs (53-55) and the bind generations (45-50) are checked on every hit. Any event that
  bumps one of them (a guest write, a texture generation change, a stream wrap) ends every entry
  that depends on it, which is the main reason stored binds are refused or flushed.
- Class 3 inputs that are not in key D and not checked on a hit are the risk: 17 (depth stencil
  flags, only the target key covers them) and 40 (mirror windows).
