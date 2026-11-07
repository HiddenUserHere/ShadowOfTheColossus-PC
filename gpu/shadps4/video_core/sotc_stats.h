// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: counters printed with the frame rate (SOTC_FPS=1, videoout/driver.cpp).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include "video_core/sotc_trace.h" // sotc: SOTC_TRACE

namespace SotcStats {
/// sotc: the switches of the stats printers in videoout/driver.cpp (SOTC_FPS line; SOTC_CMD_STATS
/// lines inside it), read once at static initialisation, before any guest thread runs (no
/// function-local static guard and no getenv inside the vectored fault handler; same pattern as
/// BbStats::enabled). Counters that only one printer reads are bumped only when that printer is
/// on: commands (SOTC_FPS) and cmd_kinds, render_breaks, upload_bytes, fault_writes,
/// fault_walker, fault_gpu_reads, fault_granule_pages (SOTC_CMD_STATS). By default the per-draw lock xadds and the
/// guest-thread fault counter (which shared cache lines with CP counters) are gone; with the
/// switches on, the printed numbers are unchanged (the printer reads and resets each counter with
/// exchange(0) only under the same switch). The variables are never set at run time.
// sotc: SOTC_TRACE (sotc_trace.h) does not turn this on. The trace spans read the clock on their
// own (SotcTrace::kOn checks at the call sites: DrawWait, RoleTimer, liverpool.cpp, vk_recorder.cpp)
// and the counters stay off unless SOTC_FPS is set. kFpsLog reads no trace state, so the order
// of the initialisation of sotc_trace.h's kOn does not matter here.
// sotc: SOTC_FRAME_METRICS (sotc_frame_metrics.h) reads these counters too, so it turns them on.
inline const bool kFpsLog = std::getenv("SOTC_FPS") != nullptr ||
                            std::getenv("SOTC_FRAME_METRICS") != nullptr;
inline const bool kCmdStats = std::getenv("SOTC_CMD_STATS") != nullptr;
/// sotc: SOTC_STALL_DETAIL=1 (off by default): the SOTCSTALL line also shows the flip pacing of the
/// frame, the guest threads' CPU and the user/kernel CPU of the upload threads (vk_presenter.cpp).
inline const bool kStallDetail = [] {
    const char* v = std::getenv("SOTC_STALL_DETAIL");
    return v && v[0] == '1';
}();
/// sotc: SOTC_FPS_LEAN=1 (off by default): SOTC_FPS keeps its per-frame summary and counters
/// but drops the per-op work that only feeds detailed reports: the per-op/per-wait clock reads
/// (kFpsTimers) and the per-binding write-extent marks (kWriteMarks, the "SOTC GPU writes by
/// shader" lines). Unset: both follow kFpsLog exactly as before. kFpsTimers/kWriteMarks read
/// kFpsLog and kFpsLean: inline variables defined earlier in this same header, which every
/// translation unit sees in this order, so they are initialized first (partial ordering).
inline const bool kFpsLean = [] {
    const char* v = std::getenv("SOTC_FPS_LEAN");
    return v && v[0] == '1';
}();
// sotc: read from the environment, not from kFpsLog/kFpsLean: the partial ordering of inline variables
// did not hold in this build (ThinLTO, static library), and kFpsTimers came out false with SOTC_FPS set
// ("SOTC FPS switches: log 1 lean 0 timers 0", 1 Nov), so every timer it gates read 0.
// sotc: SOTC_FRAME_METRICS needs the draw thread's wait times (DrawWait) for its draw_busy column,
// so it turns kFpsTimers on too. kWriteMarks keeps the SOTC_FPS switch alone (its per-binding
// marks would add work to the frames being measured).
inline bool FpsTimersFromEnv() {
    const char* lean = std::getenv("SOTC_FPS_LEAN");
    return (std::getenv("SOTC_FPS") != nullptr || std::getenv("SOTC_FRAME_METRICS") != nullptr) &&
           !(lean && lean[0] == '1');
}
inline bool FpsMarksFromEnv() {
    const char* lean = std::getenv("SOTC_FPS_LEAN");
    return std::getenv("SOTC_FPS") != nullptr && !(lean && lean[0] == '1');
}
inline const bool kFpsTimers = FpsTimersFromEnv();
inline const bool kWriteMarks = FpsMarksFromEnv(); // sotc: same switches as kFpsTimers, minus the metrics
/// sotc: skip BindResources on consecutive draws whose pipeline, push data and resource
/// handles did not change. On by default (SOTC_FAST_BIND=0 turns it off). In 0.60.5 it left black
/// dots in a grid on sandy ground (new constants at the same address); with the fix (mem_epoch
/// below) the sand under the title demo's bridge is clean (attract shots, 23 Oct). Shrine idle
/// 33.5 -> 35.3 FPS, title demo 50.6 -> 52.2.
inline const bool kFastBind = [] {
    const char* v = std::getenv("SOTC_FAST_BIND");
    return !v || v[0] != '0';
}();
/// sotc: SOTC_FASTBIND_WIDE=1 (default off): a reused bind is also taken in a later GPU submit
/// (the tick must differ). The lookups are skipped; push constants and descriptors are issued
/// again into the current command buffer (Rasterizer::BindResources, vk_rasterizer.cpp).
inline const bool kFastBindWide = [] {
    const char* v = std::getenv("SOTC_FASTBIND_WIDE");
    return v && v[0] == '1';
}();
/// sotc: SOTC_BIND_CACHE=1 (default off): a multi-entry cache of full binds of read-only graphics
/// draws, so a draw that repeats the state of an earlier draw in the same submit (not only the
/// one right before it, as SOTC_FAST_BIND) skips the buffer/texture lookups
/// (Rasterizer::TryBindCache, vk_rasterizer.cpp). Unset or 0: the bind path is unchanged.
inline const bool kBindCache = [] {
    const char* v = std::getenv("SOTC_BIND_CACHE");
    return v && v[0] == '1';
}();
/// sotc: SOTC_BIND_CACHE counters (SOTC_FPS only): eligible draws looked up (fast-bind hits are
/// not), hits, matches refused at validation (an image dirty, re-bound or a target), full binds
/// not stored (something a hit could not repeat), and flushes of a non-empty cache by reason.
inline std::atomic<uint64_t> bind_cache_lookups{0}, bind_cache_hits{0}, bind_cache_refused{0},
    bind_cache_uncacheable{0};
enum BindCacheFlushReason : uint32_t {
    BindCacheTick, BindCacheMemory, BindCacheResources, BindCacheTargets, BindCacheUnsafe,
    BindCacheReasons
};
inline constexpr const char* kBindCacheFlushNames[BindCacheReasons] = {
    "tick", "memory", "resources", "targets", "unsafe"};
inline std::atomic<uint64_t> bind_cache_flushes[BindCacheReasons]{};
/// sotc: SOTC_BIND_CACHE eligibility counters (SOTC_FPS only). Rasterizer::BindCacheRefusal names the
/// first gate a bind fails; BindResources counts each op once: heap-dropped, or refused by a gate, or
/// eligible (then fast-served, or consulted). Post-gate drops are counted on top of those.
enum BindGate : uint32_t {
    BgNone, BgCompute, BgClip, BgStorage, BgDispatchDma, BgShared, BgWrittenBuf, BgWrittenImg,
    BgPacket, BgPacketHeap, BgGates
};
inline constexpr const char* kBindGateNames[BgGates] = {
    "none", "compute", "clip plane", "storage/fmask", "dispatch dma", "shared mem",
    "written buf", "written img", "packet", "packet+heap"};
inline std::atomic<uint64_t> bind_ops{0}, bind_eligible{0}, bind_consulted{0},
    bind_fast_eligible{0}, bind_fast_refused{0}, bind_heap_dropped{0}, bind_post_copy{0},
    bind_post_nodata{0};
inline std::atomic<uint64_t> bind_gate_refused[BgGates]{};
/// Call sites of BindCacheFlush(BindCacheUnsafe), by the bind that flushed.
enum BindUnsafeSite : uint32_t {
    UsCopy, UsMetaClear, UsImageClear, UsSyncIneligible, UsSyncWriteEpoch, UsSites
};
inline constexpr const char* kBindUnsafeSiteNames[UsSites] = {
    "copy", "meta clear", "image clear", "sync ineligible", "sync write epoch"};
inline std::atomic<uint64_t> bind_unsafe_sites[UsSites]{};
/// sotc: SOTC_BIND_CACHE_RANGES=1 (default off; only with SOTC_BIND_CACHE=1): a dispatch, draw or
/// DMA that writes GPU memory drops only the cached binds that read what it writes
/// (Rasterizer::BindCacheInvalidate), and guest memory changes (mem_epoch) no longer flush the
/// cache: each hit already refuses CPU-modified buffer ranges and dirty images. Unset or 0: the
/// SOTC_BIND_CACHE flush rules are unchanged.
inline const bool kBindCacheRanges = [] {
    const char* cache = std::getenv("SOTC_BIND_CACHE");
    const char* v = std::getenv("SOTC_BIND_CACHE_RANGES");
    return cache && cache[0] == '1' && v && v[0] == '1';
}();
/// sotc: SOTC_BIND_CACHE_RANGES counters (SOTC_FPS only): entries dropped by written ranges, and
/// the writes checked against a non-empty cache.
inline std::atomic<uint64_t> bind_cache_range_entries{0}, bind_cache_range_writes{0};
/// sotc: SOTC_BIND_CACHE_META (default 1, needs SOTC_BIND_CACHE_RANGES; 0: a metadata clear flushes the whole cache as
/// before). A metadata clear that the bind path skips writes no memory and changes no image; it only moves the clear
/// state of a surface (TextureCache::ClearMeta), which no cached bind reads. The cache is therefore left alone except
/// for the binds reading the cleared surface's own range (Rasterizer::BindCacheNoteMetaClear).
inline const bool kBindCacheMeta = [] {
    const char* v = std::getenv("SOTC_BIND_CACHE_META");
    return !v || v[0] != '0';
}();
/// sotc: SOTC_BIND_CACHE_META counters (SOTC_FPS only): metadata clears narrowed, the ones that would have flushed a
/// non-empty cache (flushes avoided), and the entries that read the cleared range (entries killed, counted by
/// overlap; in the frame cache they are refused by their stamps on the next lookup).
inline std::atomic<uint64_t> meta_clear_narrowed{0}, meta_clear_flushes_avoided{0},
    meta_clear_entries_killed{0};
/// sotc: SOTC_OP_COMPUTE_BIND (default 0; needs SOTC_BIND_CACHE, _RANGES, _XTICK and _FRAME, read
/// here on their own, not from the other inline flags). 1: a dispatch with no storage image, no DMA
/// pointer, no sharp table and no shared memory takes the frame bind cache, and so does a bind whose
/// written buffers are plain guest buffers (WrittenBindAdmitted: not texel, GDS or shader-addressed).
/// A hit replays the descriptors, barriers, buffer/image lookups and the written bindings' side
/// effects (ObtainBuffer, texture invalidation, host write note, range stamps: Rasterizer::ReplayOwnWrites).
/// Written images, storage images and fmasks stay refused. 2 (verify): no hit is taken; each eligible
/// bind whose entry would have been a hit is compared with the full bind (descriptors, push data, and
/// the written-binding calls, WriteVerify). Stamps are not checked by the verify, so a stale entry can
/// show as a mismatch; a mismatch on a fresh entry is a real difference.
inline const int kOpComputeBind = [] {
    const auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    };
    if (!(on("SOTC_BIND_CACHE") && on("SOTC_BIND_CACHE_RANGES") && on("SOTC_BIND_CACHE_XTICK") &&
          on("SOTC_BIND_CACHE_FRAME"))) {
        return 0;
    }
    const char* v = std::getenv("SOTC_OP_COMPUTE_BIND");
    return v && v[0] == '2' ? 2 : (v && v[0] == '1' ? 1 : 0);
}();
/// sotc: SOTC_OP_COMPUTE_BIND counters (SOTC_FPS only): dispatches that were eligible for the
/// cache, hits taken, verify checks (would-be hits compared) and verify mismatches.
inline std::atomic<uint64_t> op_compute_eligible{0}, op_compute_hits{0},
    op_compute_verify_checks{0}, op_compute_verify_mismatches{0};
/// sotc: SOTC_OP_COMPUTE_BIND written binds (SOTC_FPS only). wr_recorded: full binds that recorded a written buffer
/// binding (and the dispatches among them in wr_compute_recorded); wr_stored: those stored in the frame cache;
/// wr_hits: hits that replayed their written bindings (Rasterizer::ReplayOwnWrites) and the dispatch ones.
inline std::atomic<uint64_t> wr_recorded{0}, wr_compute_recorded{0}, wr_stored{0}, wr_compute_stored{0},
    wr_hits{0}, wr_compute_hits{0};
/// The categories of the written-binding verify (mode 2): the write calls a replay would make against the calls the
/// full bind made (number of writes, range, narrowing mark or dispatch, bound buffer and offset, note calls).
enum WrVerifyCategory : uint32_t { WvCount, WvRange, WvMark, WvBinding, WvNote, WvCategories };
inline constexpr const char* kWrVerifyNames[WvCategories] = {"count", "range", "mark", "binding", "note"};
inline std::atomic<uint64_t> wr_verify_checks{0}, wr_verify_mismatch[WvCategories]{};
/// sotc: SOTC_PACKET counters (SOTC_FPS only): the packet ops (pipelines with the push packet) served by the frame
/// bind cache, by the fast bind (same tick or wide), and by a full bind. Their sum is the packet ops bound.
inline std::atomic<uint64_t> packet_cache_hits{0}, packet_fast_hits{0}, packet_full_binds{0};
/// sotc: SOTC_BIND_CACHE_XTICK=1 (default off; needs SOTC_BIND_CACHE=1 and SOTC_BIND_CACHE_RANGES=1):
/// cached binds survive the submit tick (the next hit of an entry in a new tick replays the
/// per-tick side effects a full bind has: image and sampler stamps, arena block stamps), the
/// buffer side of the validity checks follows BufferCache::BindVersion (removals and handle
/// changes only, not additions), and a CPU-modified buffer range is synchronized instead of
/// refusing the hit (Rasterizer::TryBindCache). Unset or 0: unchanged.
inline const bool kBindCacheXtick = [] {
    // sotc: reads every env it depends on itself instead of kBindCacheRanges: the dynamic
    // initialization order of inline variables across translation units is not reliable here,
    // and this switch came out false in every run that set it (no "xtick hits" in the logs).
    const auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    };
    return on("SOTC_BIND_CACHE") && on("SOTC_BIND_CACHE_RANGES") && on("SOTC_BIND_CACHE_XTICK");
}();
/// sotc: SOTC_BIND_CACHE_XTICK counters (SOTC_FPS only): hits of entries made in an earlier tick,
/// hits that synchronized a CPU-modified range, first hits of an entry in a new tick (replays),
/// and lookups whose key was also seen in the previous game frame.
inline std::atomic<uint64_t> bind_cache_xtick_hits{0}, bind_cache_xtick_repaired{0},
    bind_cache_xtick_replays{0}, bind_cache_seen_prev_frame{0};
/// sotc: SOTC_BIND_CACHE_FRAME=1 (default off; needs SOTC_BIND_CACHE_XTICK=1, so also _RANGES and
/// SOTC_BIND_CACHE): the bind cache survives the frame. A table of 4096 entries keyed by pipeline,
/// FastBindHash and the render targets (not flushed when they change); an entry is checked at its
/// hit against what it depends on instead of being wiped by a global generation: the images
/// registered or unregistered over the ranges it looked up (TextureCache::ImageChangedSince), the
/// GPU writes to what it reads (RangeStamps), BufferCache::BindVersion and, with flat buffers, the
/// stream ring. SOTC_FPS counters: misses by reason (a lookup that found no usable entry), entries
/// dropped by reason, stores, hits that crossed a game frame, and the buffer binding memo.
/// Unset or 0: the SOTC_BIND_CACHE path is unchanged.
inline const bool kBindCacheFrame = [] {
    const auto on = [](const char* name) { // sotc: see kBindCacheXtick
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    };
    return on("SOTC_BIND_CACHE") && on("SOTC_BIND_CACHE_RANGES") && on("SOTC_BIND_CACHE_XTICK") &&
           on("SOTC_BIND_CACHE_FRAME");
}();
/// sotc: SOTC_BUFFER_MEMO=1 (off by default): only the buffer binding memo of SOTC_BIND_CACHE_FRAME
/// (vk_rasterizer.cpp BufferMemo: a read-only V# of a read-only graphics bind reuses the arena and
/// offset ObtainBuffer gave for the same address, size and mapping while BindVersion is unchanged;
/// CPU-written ranges are still synchronized and the use stamps made). The rest of the frame-wide
/// bind cache (which measured worse) stays off. Needs SOTC_BIND_CACHE (the read-only eligibility).
inline const bool kBufferMemo = [] {
    const char* v = std::getenv("SOTC_BUFFER_MEMO");
    return v && v[0] == '1';
}();
enum BindFrameReason : uint32_t {
    BfNoEntry,   ///< no entry under the key
    BfFlush,     ///< made before a cache-wide flush (sampler destroyed, metadata clear)
    BfBuffers,   ///< BufferCache::BindVersion moved (an unbind, eviction or arena replacement)
    BfStream,    ///< the stream ring wrapped under an entry that holds a flat buffer copy
    BfWrites,    ///< a GPU write touched a range or image the entry reads
    BfImages,    ///< an image was registered/unregistered over a looked-up range
    BfMapping,   ///< a guest map/unmap with an entry that held a rejected (unmapped) T#
    BfRefused,   ///< the hit-time checks (dirty / re-bound image, mapped extent, sampler)
    BfReasons
};
inline constexpr const char* kBindFrameNames[BfReasons] = {"none", "flush",  "buffers", "stream",
                                                           "writes", "images", "mapping", "refused"};
inline std::atomic<uint64_t> bf_miss[BfReasons]{}, bf_drop[BfReasons]{};
/// sotc: SOTC_BIND_CACHE_FRAME. The site that refused a hit (BfRefused), by the check it failed. Counted with
/// bf_miss[BfRefused]; the sites are the refuse calls of Rasterizer::TryBindCacheFrame (SOTC_OPSET measurements).
enum BindRefuseSite : uint32_t {
    RfBindless,     ///< the bindless heap generation moved (a slot released or evicted)
    RfFlatSize,     ///< a flat buffer of the entry changed size (the stage's words no longer fit the copy)
    RfMapReplay,    ///< a repair's guest mapping changed before the entry was replayed
    RfMapRepair,    ///< a repair's guest mapping changed while it was synchronized
    RfFlatTick,     ///< a flat copy left its stream lap or tick during the repair
    RfMapAfter,     ///< the guest mapping changed after the repair (second check of the replay)
    RfSampler,      ///< a sampler of the entry is no longer the one the sampler cache returns
    RfImage,        ///< an image of the entry is dirty, unregistered, re-bound, a target or force-general
    RfHeapDead,     ///< sotc: SOTC_BIND_CACHE_HEAP, a heap resolution the entry holds is no longer live (its BindSet killed)
    RfHeapDecline,  ///< sotc: SOTC_BIND_CACHE_HEAP, the held heap images are declined now (HeapDeclineReason)
    RfHeapShape,    ///< sotc: SOTC_BIND_CACHE_HEAP, the entry's heap stages are not the op's (should not happen)
    RfHeapPublish,  ///< sotc: SOTC_BIND_CACHE_HEAP, the index table ring section is full
    RfWriteReplay,  ///< sotc: SOTC_OP_COMPUTE_BIND: a written binding of the entry binds another buffer or offset now
    RfSites
};
inline constexpr const char* kBindRefuseSiteNames[RfSites] = {
    "bindless", "flatsize", "map", "maprepair", "flattick", "mapafter", "sampler", "image",
    "heapdead", "heapdecline", "heapshape", "heappublish", "write"};
inline std::atomic<uint64_t> bf_refuse[RfSites]{};
inline std::atomic<uint64_t> bf_stored{0}, bf_replaced{0}, bf_not_stored_moved{0},
    bf_cross_frame_hits{0}, bf_narrowed{0};
/// sotc: SOTC_BIND_CACHE_FRAME buffer binding memo (BindBuffers): read-only V#s that skipped
/// ObtainBuffer, those that ran it, and the memo's repairs of CPU-modified ranges.
inline std::atomic<uint64_t> bf_memo_hits{0}, bf_memo_misses{0}, bf_memo_repaired{0};
/// sotc: SOTC_BIND_CACHE_FRAME resource identity key: flat buffer copies a hit made / found still
/// current, distinct keys looked up and game frames seen (distinct keys per frame: does the table
/// hold a frame's worth?), and the entries live in the table.
inline std::atomic<uint64_t> bf_flat_copied{0}, bf_flat_reused{0}, bf_distinct_keys{0},
    bf_frames{0}, bf_live{0};
/// sotc: SOTC_BIND_CACHE_PACKET (default 0; needs SOTC_BIND_CACHE, _RANGES, _XTICK and _FRAME, read here on their own).
/// 1: the packet ops of heap pipelines (profile heap_images 1) take the frame bind cache too: a hit re-resolves nothing
/// of the heap table (Rasterizer::BindResources resolves it before the lookup) but binds the heap images and the heap
/// block of the op; and the key of a packet op leaves out the base address of its shader-addressed V#s, a hit
/// refreshes their ranges from the op's own V#s (Rasterizer::PacketAddrRefresh). 2 (verify): the same, but a would-be
/// hit is never taken: the full bind runs and its result is compared with the entry (Rasterizer::PacketVerify).
/// Unset or 0: unchanged.
inline const int kBindCachePacket = [] {
    const auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    };
    if (!(on("SOTC_BIND_CACHE") && on("SOTC_BIND_CACHE_RANGES") && on("SOTC_BIND_CACHE_XTICK") &&
          on("SOTC_BIND_CACHE_FRAME"))) {
        return 0;
    }
    const char* v = std::getenv("SOTC_BIND_CACHE_PACKET");
    return v && v[0] == '2' ? 2 : (v && v[0] == '1' ? 1 : 0);
}();
/// sotc: SOTC_BIND_CACHE_CONTENT (default 0; needs the SOTC_BIND_CACHE_FRAME switches, read here on their own). 0: a GPU
/// write to a range or image an entry reads drops it (today's rule). 1: a GPU write drops an entry only when it changes the
/// GPU-modified state of an image over one of the entry's lookup ranges (what FindImage's mip-tail choice reads, see
/// VideoCore::GpuFlagStamps); the other writes change no bind result, and a hit replays the image transitions and buffer
/// barriers as it always did. 2 (verify): today's rule decides every bind: a would-be hit is not taken, the entry is held
/// for the comparison, and the full bind's result is compared with it (Rasterizer::BindCacheVerify) by category.
/// Unset or 0: unchanged.
inline const int kBindCacheContent = [] {
    const auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    };
    if (!(on("SOTC_BIND_CACHE") && on("SOTC_BIND_CACHE_RANGES") && on("SOTC_BIND_CACHE_XTICK") &&
          on("SOTC_BIND_CACHE_FRAME"))) {
        return 0;
    }
    const char* v = std::getenv("SOTC_BIND_CACHE_CONTENT");
    return v && v[0] == '2' ? 2 : (v && v[0] == '1' ? 1 : 0);
}();
/// sotc: SOTC_BIND_CACHE_CONTENT counters (SOTC_FPS only). Rules that refused a hit: the GPU-modified state of an image
/// over a lookup range (flag), and mode 2's held entries (verify: today's rule would drop it). Verify categories: the
/// descriptors, the push data, the bound buffer records, the image records, the other bind records (LOD banks, samplers,
/// bindless), and the watched ranges (which change no result).
enum BindContentRule : uint32_t { CrFlag, CrHold, CrRules };
inline constexpr const char* kBindContentRuleNames[CrRules] = {"flag", "verify-hold"};
enum BindContentDiff : uint32_t { CdDescriptors, CdPush, CdBuffers, CdImages, CdOther, CdRanges, CdCats };
inline constexpr const char* kBindContentDiffNames[CdCats] = {
    "descriptors", "push", "buffers", "images", "other", "ranges"};
inline std::atomic<uint64_t> bc_kept{0}, bc_refused[CrRules]{}, bc_verified{0},
    bc_skipped_moved{0}, bc_diff[CdCats]{}, bc_diff_binds{0};
/// sotc: SOTC_BIND_CACHE_PACKET counters (SOTC_FPS only): packet ops that reached the bind, the eligible ones, hits,
/// misses by reason (the TryBindCacheFrame reasons, then the heap declined and the addressed shape changed), verify
/// checks (mode 2), and the verify mismatches by category.
enum BindPacketMiss : uint32_t { PkHeapDeclined = BfReasons, PkAddrShape, PkMissReasons };
inline constexpr const char* kBindPacketMissNames[PkMissReasons] = {
    "no entry", "flush", "buffers", "stream", "writes", "images", "mapping", "refused",
    "heap declined", "addr shape"};
inline std::atomic<uint64_t> pk_ops{0}, pk_eligible{0}, pk_hits{0}, pk_verify_checks{0};
inline std::atomic<uint64_t> pk_miss[PkMissReasons]{};
/// PvImgInfo counts the image infos a hit would replay wrongly (image, sampler, view or layout). The four PvImg*
/// categories classify every differing image info (each counted once per op): a different image; the same image
/// with a view of another backing (PvImgBacking: a hit now refuses it); a view not owned by any backing of the
/// image (PvImgView); the same view with another layout (PvImgLayout). PvImgSampler: a sampler handle changed.
enum PacketVerifyCategory : uint32_t {
    PvWrites, PvBufInfo, PvImgInfo, PvPush, PvAddr, PvBuffers, PvImages,
    PvImgImage, PvImgSampler, PvImgView, PvImgLayout, PvImgBacking, PvCategories
};
inline constexpr const char* kPacketVerifyNames[PvCategories] = {
    "writes", "buffer infos", "image infos", "push", "addr", "buffers", "images",
    "img image", "img sampler", "img view", "img layout", "img backing"};
inline std::atomic<uint64_t> pk_mismatch[PvCategories]{};
/// sotc: SOTC_RS_MEMO=1 (default off): a draw into the same render targets as the draw before it,
/// in the render pass that draw began, takes that draw's render state instead of looking its
/// targets up again (Rasterizer::RsMemoPrepare / RsMemoBegin, vk_rasterizer.cpp). Unset or 0:
/// PrepareRenderState and BeginRendering are unchanged.
/// sotc: SOTC_RS_MEMO=2 also takes the memo of an earlier submit (the tick differs): the targets
/// must still be the same registered images (uid and backing), the metadata epoch must not have
/// moved and the transitions that are not provably no-ops are run. =3 runs the full path on every
/// candidate and compares the result with the memo (mismatches are counted; the full path's result
/// is used). Each inline variable below reads the environment once at static initialization (no
/// inline variable depends on another one).
inline unsigned RsMemoLevel() {
    const char* v = std::getenv("SOTC_RS_MEMO");
    return (v && v[0] >= '1' && v[0] <= '3') ? static_cast<unsigned>(v[0] - '0') : 0u;
}
inline const bool kRsMemo = RsMemoLevel() >= 1;
inline const bool kRsMemoCross = RsMemoLevel() >= 2;
inline const bool kRsMemoVerify = RsMemoLevel() == 3;
/// sotc: SOTC_RS_MEMO counters (SOTC_FPS only): draws looked up, hits, misses by reason, and full
/// passes not stored (a clear, a feedback loop). Mode 2: hits that took the memo of an earlier
/// submit. Mode 3: candidates verified against the full path, and the ones that differed.
enum RsMemoMissReason : uint32_t {
    RsMissNone, RsMissTick, RsMissResources, RsMissRegs, RsMissPass, RsMissImage, RsMissMeta,
    RsMissIdentity, RsMissReasons
};
inline constexpr const char* kRsMemoMissNames[RsMissReasons] = {
    "empty", "tick", "resources", "regs", "pass", "image", "meta", "identity"};
inline std::atomic<uint64_t> rs_memo_lookups{0}, rs_memo_hits{0}, rs_memo_unstored{0};
inline std::atomic<uint64_t> rs_memo_cross_hits{0}, rs_memo_verify_checks{0},
    rs_memo_verify_mismatches{0};
inline std::atomic<uint64_t> rs_memo_misses[RsMissReasons]{};
/// sotc: SOTC_PARALLEL_RECORD=1 (default off): GpuPrep threads compute the register-only inputs
/// of the draws ahead of the draw thread (Liverpool::PrepLoop, amdgpu/draw_prep.h). Counters
/// (SOTC_FPS only): draws that took a prepared slot, draws computed inline because the slot was
/// not published yet, a mapping changed since, or the prep thread refused the draw; the largest
/// distance (ops) between the newest queued op and the slowest prep thread; the time the command
/// processor waited for a full ring because of the prep threads; SOTC_PR_VERIFY differences.
inline std::atomic<uint64_t> pr_prepared{0}, pr_not_ready{0}, pr_epoch{0}, pr_ineligible{0},
    pr_throttle_ns{0}, pr_verify_mismatches{0};
inline std::atomic<uint32_t> pr_lag_max{0};
/// sotc: SOTC_PR_KEYS (draw_prep.h PreparedDraw::table_keys, TableMemo::ComputeKey). precomputed: heap table keys a
/// GpuPrep thread computed (SOTC_FPS only); used: TableMemo::Resolve replayed one instead of KeyOf (SOTC_FPS only);
/// mismatch: SOTC_VERIFY differences between the record and the draw thread's KeyOf (always counted).
inline std::atomic<uint64_t> pr_keys_precomputed{0}, pr_keys_used{0}, pr_keys_mismatch{0};
/// sotc: SOTC_HOST_IMAGE_COPY=1 (default off): linear textures the GPU did not write are re-uploaded
/// by the host (vkCopyMemoryToImageEXT, VK_EXT_host_image_copy) instead of a staging copy in a
/// command buffer (TextureCache::RefreshImage, Runtime::TryHostImageCopy). Off as well when the
/// texture dump or replacement hook is set (they change the bytes after the upload) and with
/// SOTC_FASTBIND_WIDE=1 (a bind reused in a later submit is not seen by the image's tick).
inline const bool kHostImageCopy = [] {
    const char* v = std::getenv("SOTC_HOST_IMAGE_COPY");
    return v && v[0] == '1' && !std::getenv("SOTC_TEX_DUMP") && !std::getenv("SOTC_TEX_REPLACE") &&
           !(std::getenv("SOTC_FASTBIND_WIDE") && std::getenv("SOTC_FASTBIND_WIDE")[0] == '1');
    // sotc: reads SOTC_FASTBIND_WIDE itself (see kBindCacheXtick on initialization order).
}();
/// sotc: host uploads done, and candidates refused because the image was not idle on the GPU.
inline std::atomic<uint64_t> host_image_copies{0};
inline std::atomic<uint64_t> host_image_copy_busy{0};
inline std::atomic<uint64_t> fast_bind_eligible{0};
inline std::atomic<uint64_t> fast_bind_hits{0};
inline std::atomic<uint64_t> fast_bind_wide_hits{0}; ///< SOTC_FASTBIND_WIDE: reused in a later submit
/// sotc: eligible draws that rebuilt their bindings (SOTC_FPS only, BindResources): all misses,
/// and the misses where only the submit tick differed (the state was otherwise reusable).
inline std::atomic<uint64_t> fast_bind_misses{0};
inline std::atomic<uint64_t> fast_bind_tick_misses{0};
/// sotc: fast-bind misses that only a dispatch in between caused (same tick / another tick), SOTC_FPS only.
inline std::atomic<uint64_t> fast_bind_compute_misses{0}, fast_bind_compute_tick_misses{0};
/// sotc: SOTC_TEX_REPEAT=1: texture binds, and the ones repeating a T# of the same submit / frame (vk_rasterizer.cpp).
inline std::atomic<uint64_t> tex_repeat_binds{0}, tex_repeat_tick_hits{0}, tex_repeat_frame_hits{0}, tex_repeat_ticks{0};
/// sotc: bumped whenever guest memory changes outside the GPU's own draws: a write by the command
/// processor (constant RAM dumps, WRITE_DATA, DMA: MemoryManager::NoteHostWrite) or a CPU write
/// the page tracking sees (MemoryTracker). A reused bind (SOTC_FAST_BIND) needs it unchanged: the
/// next draw may have the same descriptors over new constants at the same address, and skipping
/// BindResources skipped their upload (black dots in a grid on the sand, user, 0.60.5).
inline std::atomic<uint64_t> mem_epoch{0};
/// sotc: SOTC_SYNC_FAST=1 (default off). BufferCache::SynchronizeMemory runs for every buffer of
/// every draw and dispatch and scans the page bits of the range even when nothing changed since
/// that range was last synchronized. With it on, each RegionManager keeps a write generation per
/// 2 MiB (RegionManager::sync_gen, bumped after any CPU bit rises or GPU bit falls), and a range
/// found clean after a sync is skipped in O(1) while the generations of its granules have not
/// moved (BufferCache::SynchronizeMemory). Unset or 0: the generations are never bumped and the
/// sync path is unchanged. Read at static initialisation (the bump runs in the fault handler).
inline const bool kSyncFast = [] {
    const char* v = std::getenv("SOTC_SYNC_FAST");
    return v && v[0] == '1';
}();
/// sotc: SOTC_SYNC_FAST counters (SOTC_FPS only): syncs looked up, syncs skipped.
inline std::atomic<uint64_t> sync_fast_lookups{0}, sync_fast_skipped{0};
/// sotc: SOTC_PROTECT_BATCH=1 (default off). The re-protects of one buffer upload
/// (BufferCache::SynchronizeMemoryFull: CPU bits cleared, pages write-protected again) are
/// collected per thread and applied together before the upload reads guest memory: adjacent runs,
/// also across 16 MiB tracking regions, become one host protect, and pages whose tracker state
/// changed meanwhile are skipped (PageManager::ProtectBatch). Unset or 0: every update protects
/// at once, as before.
inline const bool kProtectBatch = [] {
    const char* v = std::getenv("SOTC_PROTECT_BATCH");
    return v && v[0] == '1';
}();
/// sotc: SOTC_PROTECT_BATCH counters (SOTC_FPS only): host protect calls the flushes made and the
/// pages they covered, protect requests collected (one per call made without batching), pages
/// skipped because their state had moved on.
inline std::atomic<uint64_t> protect_batch_calls{0}, protect_batch_pages{0},
    protect_batch_requests{0}, protect_batch_redundant{0};
/// sotc: SOTC_SYNC_PIPE=1 (default off; =2 also verifies). Read at static initialisation.
///  1. BufferCache::SynchronizeMemory (needs SOTC_SYNC_FAST=1) takes the clean verdict of its own
///     scan (MemoryTracker::ForEachUploadRange) instead of a second look at the bits.
///  2. The SRT clean-page bitmap (SetSrtReadProtectedPages) is updated one 64-bit word at a time.
///  3. Counters of the Protect calls made inside Rasterizer::BindResources (SyncPipe::OpScope),
///     printed to stderr every 5 s.
/// =2 computes both verdicts of (1) and (2), counts the disagreements in verify_mismatches, and
/// uses the old result.
inline const bool kSyncPipe = [] {
    const char* v = std::getenv("SOTC_SYNC_PIPE");
    return v && (v[0] == '1' || v[0] == '2');
}();
inline const bool kSyncPipeVerify = [] {
    const char* v = std::getenv("SOTC_SYNC_PIPE");
    return v && v[0] == '2';
}();
namespace SyncPipe {
/// sotc: SOTC_SYNC_PIPE counters, cumulative: ops (BindResources calls) with at least one Protect
/// call, Protect calls made inside BindResources, ops with two or more of them, and verify
/// mismatches (=2).
inline std::atomic<uint64_t> ops_with_protects{0}, protects{0}, ops_two_plus{0},
    verify_mismatches{0};
/// Per thread: the BindResources nesting depth and the Protect calls of the op in progress.
inline thread_local uint32_t bind_depth = 0;
inline thread_local uint32_t op_protects = 0;
/// Steady-clock time (ns) of the next report; 0 until the first op ends (that call only arms it).
inline std::atomic<int64_t> next_report_ns{0};

/// Counts one Protect call, when the calling thread is inside BindResources.
inline void NoteProtect() noexcept {
    if (kSyncPipe && bind_depth != 0) {
        ++op_protects;
        protects.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Prints the counters to stderr, at most once per 5 s.
inline void PrintReport() {
    constexpr int64_t period_ns = 5'000'000'000;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    int64_t next = next_report_ns.load(std::memory_order_relaxed);
    if (next == 0) {
        next_report_ns.compare_exchange_strong(next, now + period_ns, std::memory_order_relaxed);
        return;
    }
    if (now < next ||
        !next_report_ns.compare_exchange_strong(next, now + period_ns, std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr,
                 "SOTC SYNC PIPE: ops with protects %llu, protects %llu, ops with 2+ %llu, "
                 "verify mismatches %llu\n",
                 static_cast<unsigned long long>(ops_with_protects.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(protects.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(ops_two_plus.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     verify_mismatches.load(std::memory_order_relaxed)));
}

/// One BindResources call: counts its Protect calls and reports when the outermost one ends.
struct OpScope {
    OpScope() noexcept {
        if (kSyncPipe && bind_depth++ == 0) {
            op_protects = 0;
        }
    }
    ~OpScope() {
        if (kSyncPipe && --bind_depth == 0) {
            if (op_protects != 0) {
                ops_with_protects.fetch_add(1, std::memory_order_relaxed);
            }
            if (op_protects >= 2) {
                ops_two_plus.fetch_add(1, std::memory_order_relaxed);
            }
            PrintReport();
        }
    }
    OpScope(const OpScope&) = delete;
    OpScope& operator=(const OpScope&) = delete;
};
} // namespace SyncPipe
/// sotc: SOTC_PROTECT_COALESCE=1 (default off). Two exact reductions of host protect calls (page_manager.cpp):
///  1. A guest write fault collects the unprotects its tracker updates make (the faulting page, the
///     fault granule, the write-ahead, an image's pages) and applies them as one call per run when
///     the handler is done, instead of one call per update.
///  2. The flush of a ProtectBatch or of a fault bridges holes of up to SOTC_PROTECT_GAP pages
///     (default 8) between runs of the same protection when the pages in the hole are in that
///     protection already (checked under their page locks).
/// See PageManager::FaultProtectScope for why neither can hide a write from the GPU.
inline const bool kProtectCoalesce = [] {
    const char* v = std::getenv("SOTC_PROTECT_COALESCE");
    return v && v[0] == '1';
}();
/// sotc: SOTC_PROTECT_COALESCE counters (SOTC_FPS only): unprotect runs collected by faults, host
/// calls and pages the fault flushes made, pages skipped because their state had moved on, and
/// faults that found their own page not covered by their runs (another thread's flush pending).
inline std::atomic<uint64_t> coalesce_requests{0}, coalesce_calls{0}, coalesce_pages{0},
    coalesce_skipped{0}, coalesce_self_fixes{0};
/// sotc: the game started loading a save or a new area ("st_gameflow() starting game",
/// runtime_file.c): the texture cache drops every image not in use (TextureCache::
/// RunGarbageCollector), so stale images over reused texture memory go away (green blocks after
/// long sessions and many loads, user, 22 Oct). SOTC_LOAD_FLUSH=0 turns it off.
inline std::atomic<bool> load_flush{false};
/// sotc: draws recorded since the process started (Rasterizer::Draw / DrawIndirect, one relaxed add each). The Lab
/// route reads its growth per flip to see the end of a load: the loading screen draws a few hundred per frame, the
/// game world thousands (gpu/sotc/lab_commands.cpp, AutoRouteTick).
inline std::atomic<uint64_t> total_draws{0};
/// sotc: F9 (diagnostic): every cached texture not drawn by the GPU is uploaded again from guest
/// memory. Garbage that stays was written there by the game; garbage that goes was a stale copy.
inline std::atomic<bool> reload_all{false};
/// sotc: F5 (diagnostic): the textures the next draws sample are saved to user/log/texcap_N
/// (raw mip 0 as the GPU holds it; Rasterizer::CaptureTextures).
inline std::atomic<bool> tex_capture{false};
/// sotc: FindMemo hits whose slot no longer held the memoized image (TextureCache::MemoLookup).
inline std::atomic<uint64_t> memo_stale{0};
/// sotc: readback bytes landing on pages the CPU wrote after the GPU (BufferCache, SOTC_RB_CPU_GUARD).
inline std::atomic<uint64_t> rb_over_cpu{0};
/// sotc: DMA copies (Rasterizer::CopyBuffer) done in host memory / on the GPU (SOTC_POOL_WATCH).
inline std::atomic<uint64_t> dma_cpu_copies{0}, dma_cpu_bytes{0}, dma_gpu_copies{0}, dma_gpu_bytes{0};
inline std::atomic<uint64_t> reuse_size_frees{0};
/// sotc: SOTC_POOL_WATCH: guest addresses of images whose pages the game freed while in use; the
/// rasterizer logs the T#s bound at these addresses afterwards.
inline std::atomic<uint64_t> pool_watch_addr[64]{};
inline std::atomic<uint32_t> pool_watch_count{0}; ///< ResolveOverlap: slot reused by another size
inline std::atomic<uint64_t> gc_downloads_skipped{0}; ///< SOTC_GC_DOWNLOAD=0
inline std::atomic<uint64_t> f9_gpu_copies{0}; ///< F9 uploads whose memory the GPU had written
/// sotc: bytes of hot readbacks not applied because the guest may have written them since.
inline std::atomic<uint64_t> rb_hot_cpu_skipped{0};
/// sotc: bytes of hot readbacks not applied because the guest wrote them during the GPU wait.
inline std::atomic<uint64_t> rb_hot_late_skipped{0};
inline std::atomic<uint64_t> readbacks{0};    ///< BufferCache::DownloadMemory calls
inline std::atomic<uint64_t> readback_ns{0};  ///< time spent in them (copy and wait)
inline std::atomic<uint64_t> watch_addr{0};     ///< SOTC_CS_WATCH: GPU writes to report
inline std::atomic<uint64_t> current_program{0}; ///< hash of the dispatch being recorded (1 = a draw)
inline std::atomic<uint64_t> readback_ahead{0};   ///< downloads of already submitted work
inline std::atomic<uint64_t> readback_dirty{0}; ///< sotc: async readbacks redone synchronously (stale)
/// sotc: in-flight readbacks made inexact, by reason (BufferCache::StaleReason: unmap, another
/// readback applied, a later GPU write, a command processor write), and the ones applied anyway
/// with the later GPU write's parts kept GPU-modified (SOTC_RB_KEEP_LATER).
inline std::atomic<uint64_t> readback_stale[4]{};
inline std::atomic<uint64_t> readback_kept_later{0};
/// sotc: SOTC_FPS only: guest threads' wall-clock time in asynchronous readbacks that waited
/// (BufferCache::ReadMemory, from the Begin request to the Finish return), since the last report:
/// count, total, longest, and the parts waiting for the submit (recording thread) and the GPU.
/// rb_guest_all_* are the same totals, never reset (FrameAccount, SOTCSTALL lines).
inline std::atomic<uint64_t> rb_guest_count{0}, rb_guest_ns{0}, rb_guest_max_ns{0};
inline std::atomic<uint64_t> rb_guest_submit_ns{0}, rb_guest_gpu_ns{0};
inline std::atomic<uint64_t> rb_guest_all_ns{0}, rb_guest_all_count{0};
/// sotc: async readbacks whose copies read nothing the tick being recorded writes, with no
/// pending sparse binds (could take SOTC_RB_ASYNC_AHEAD); those that did (SOTC_RB_ASYNC_AHEAD=1),
/// their guest waits (counted when the guest finishes, with rb_guest_*), and the eligible ones
/// left on the usual path because no slot was free. Since the last report.
inline std::atomic<uint64_t> rb_ahead_eligible{0};
inline std::atomic<uint64_t> rb_ahead_async{0}, rb_ahead_async_ns{0}, rb_ahead_async_busy{0};
/// sotc: guest readbacks by the state of the tick that wrote their bytes last (write ring of
/// BufferCache): the GPU finished it / submitted but not finished / still being recorded; the sum of
/// their age in ticks (current tick - writer tick) and their count. Since the last report.
inline std::atomic<uint64_t> rb_writer_done{0}, rb_writer_sub{0}, rb_writer_unsub{0};
inline std::atomic<uint64_t> rb_writer_age_sum{0}, rb_writer_count{0};
/// sotc: SOTC_RB_TICK: copies done on the transfer queue, the guest waits for them (count, ns), and
/// the readbacks that took the usual path: all, no transfer queue, writer unknown, binds pending.
inline std::atomic<uint64_t> rb_tick_xfer{0}, rb_tick_waits{0}, rb_tick_wait_ns{0};
inline std::atomic<uint64_t> rb_tick_fb{0}, rb_tick_fb_noxfer{0}, rb_tick_fb_unknown{0},
    rb_tick_fb_binds{0};
/// sotc: SOTC_FPS only: the pattern of the guest readback requests (BufferCache::ReadMemory, async
/// path; "SOTC RB pattern" line), since the last report. Per guest thread, a request is "adjacent"
/// when its faulting address lies outside the same thread's previous readback window but within
/// 64 KiB after its end or before its start (_1ms: and it came at most 1 ms after the previous
/// one returned), "inside" when it lies in that window (pages the previous readback left
/// protected: owned by another readback, kept GPU-modified, or written by the GPU again).
/// "repeat": its base granule (the SOTC_RB_NARROW window, 512 KiB without it) was read back in
/// the previous game frame (flip_count) too. waited: requests that waited for the GPU.
/// asked / window / downloaded: bytes the fault asked for (<= 8, the fault handler does not
/// decode the access size), of the windows, and of GPU-modified data actually copied.
/// rb_pat_adjacent_all: adjacent requests, never reset (FrameAccount, SOTCSTALL "rb adj").
inline std::atomic<uint64_t> rb_pat_count{0}, rb_pat_waited{0}, rb_pat_adjacent{0},
    rb_pat_adjacent_1ms{0}, rb_pat_inside{0}, rb_pat_repeat{0};
inline std::atomic<uint64_t> rb_pat_asked_bytes{0}, rb_pat_window_bytes{0},
    rb_pat_download_bytes{0};
inline std::atomic<uint64_t> rb_pat_adjacent_all{0};
/// sotc: SOTC_RB_GROW=1 (BufferCache::ReadMemory / BeginAsyncReadback), since the last report:
/// requests with a grown window; of those, the ones whose copies read bytes the tick being
/// recorded writes (current_writes: not eligible for SOTC_RB_ASYNC_AHEAD) and the same for base
/// windows; other base granules of the grown windows that held GPU-modified data (readbacks
/// saved, upper bound: each would have been a fault of its own if the guest reads it) and those
/// bytes. Counted with SOTC_FPS only.
inline std::atomic<uint64_t> rb_grow_grown{0}, rb_grow_cur_writes{0}, rb_base_cur_writes{0},
    rb_grow_extra_granules{0}, rb_grow_extra_bytes{0};
/// sotc: formats the distinct base granules and the top 5 granules by request count of the
/// current report period into `out` (at most out_size bytes, NUL-terminated) and starts a new
/// period. Defined in buffer_cache.cpp (the pattern table lives there).
void RbPatternTake(char* out, std::size_t out_size);
/// sotc: shader binaries of an older build replaced in the cache, and permutations left out of it
/// because their slot holds another current one (RegisterShaderBinary).
inline std::atomic<uint64_t> shader_cache_replaced{0};
inline std::atomic<uint64_t> shader_cache_left_out{0};
/// sotc: SOTC_RB_OWNER: readbacks that found part of their range downloaded by others, and the
/// bytes they left to them. SOTC_RB_BATCH: batched submits, and the submits they saved.
inline std::atomic<uint64_t> rb_owner_shared{0};
inline std::atomic<uint64_t> rb_owner_skipped_bytes{0};
/// sotc: SOTC_RB_PREFETCH ("SOTC RB prefetch" line), since the last report: prefetches recorded;
/// applied at a guest fault with their copies done (no GPU wait); still on the GPU at the fault
/// (the guest waited for their tick); freed unasked after SOTC_RB_PREFETCH_TICKS; freed because a
/// later write or unmap made them stale; windows not prefetched because of the count or staging
/// caps. rb_pf_consumed_all: never reset (FrameAccount, SOTCSTALL "rb pf").
inline std::atomic<uint64_t> rb_pf_issued{0}, rb_pf_consumed{0}, rb_pf_waited{0},
    rb_pf_expired{0}, rb_pf_stale{0}, rb_pf_capped{0};
inline std::atomic<uint64_t> rb_pf_consumed_all{0};
/// sotc: SOTC_RB_MATERIALIZE: prefetches applied without a guest fault, their bytes, and the ones
/// issued with ranges not all written by the GPU in this frame or the previous one (left to faults).
inline std::atomic<uint64_t> rb_mat_applied{0}, rb_mat_bytes{0}, rb_mat_not_fresh{0};
/// sotc: SOTC_RB_PREV: guest read faults served from the last applied version (and bytes
/// released), refused because the window is not rewritten every frame, refused because the
/// last applied version is too old. rb_prev_served_all: never reset (SOTCSTALL "prev").
inline std::atomic<uint64_t> rb_prev_served{0}, rb_prev_bytes{0}, rb_prev_not_producer{0},
    rb_prev_too_old{0};
inline std::atomic<uint64_t> rb_prev_served_all{0};
/// sotc: SOTC_RB_PREV: owed downloads recorded, and serves refused because a guest readback in
/// flight already downloads the pages.
inline std::atomic<uint64_t> rb_prev_owed{0}, rb_prev_busy{0};
/// sotc: SOTC_RB_EXACT ("SOTC RB exact" line), since the last report. served: guest read faults
/// answered without waiting for GPU work that was not done (served_dl: with a download of done
/// ranges only, the rest of served: nothing to copy). pages_owed: pages released with owed ranges;
/// owed_objs/owed_applied/owed_dropped/owed_bytes: owed downloads recorded, applied to guest
/// memory, dropped (nothing left to apply) and the bytes applied; owed_trimmed: bytes a later
/// write, unmap or guest write took out of an owed download. fb_*: faults that took the usual
/// readback, by reason (announced: a pending range already announced by a label; unknown: writer
/// not known precisely; cpu: page with the CPU bit; inflight: pages owned by a guest readback in
/// flight; other: write fault, no GPU range, caps). label_*: labels deferred by liverpool.cpp and
/// their deferral latency (RbExLabelDeferral). unsafe_announce / apply_wrongthread are
/// diagnostics that must stay 0.
inline std::atomic<uint64_t> rb_ex_served{0}, rb_ex_served_dl{0}, rb_ex_pages_owed{0},
    rb_ex_owed_objs{0}, rb_ex_owed_applied{0}, rb_ex_owed_dropped{0}, rb_ex_owed_bytes{0},
    rb_ex_owed_trimmed{0};
inline std::atomic<uint64_t> rb_ex_fb_announced{0}, rb_ex_fb_unknown{0}, rb_ex_fb_cpu{0},
    rb_ex_fb_inflight{0}, rb_ex_fb_other{0};
inline std::atomic<uint64_t> rb_ex_label_deferred{0}, rb_ex_label_defer_ns{0},
    rb_ex_label_defer_max_ns{0};
inline std::atomic<uint64_t> rb_ex_unsafe_announce{0}, rb_ex_apply_wrongthread{0};
inline std::atomic<uint64_t> rb_ex_fb_floor{0};
/// sotc: SOTC_WRITE_FAULT_NARROW: write faults narrowed to their pages, window bytes not read.
inline std::atomic<uint64_t> rb_wf_narrow{0}, rb_wf_saved{0};
inline std::atomic<uint64_t> rb_ex_apply_reentered{0}, rb_ex_settle_waits{0}, rb_ex_settle_ns{0};
/// sotc: SOTC_RB_EXACT label downloads: recorded, applied before a label, dropped (dirty or
/// unused; the pages stay protected) and the bytes applied.
inline std::atomic<uint64_t> rb_ex_ld_recorded{0}, rb_ex_ld_applied{0}, rb_ex_ld_dropped{0},
    rb_ex_ld_bytes{0};
/// sotc: SOTC_RB_EXACT: one deferred label was written ns after it was processed.
inline void RbExLabelDeferral(uint64_t ns) {
    rb_ex_label_deferred.fetch_add(1, std::memory_order_relaxed);
    rb_ex_label_defer_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t seen = rb_ex_label_defer_max_ns.load(std::memory_order_relaxed);
    while (seen < ns && !rb_ex_label_defer_max_ns.compare_exchange_weak(
                            seen, ns, std::memory_order_relaxed)) {
    }
}
/// sotc: SOTC_HRR (dry run, buffer_cache/hrr.h): would-be promoted ranges and their readbacks.
/// hrr_faults_all is never reset (SOTCSTALL "hrr"); the others are reset by the SOTC HRR line.
inline std::atomic<uint64_t> hrr_ranges{0}, hrr_kib{0}, hrr_faults{0}, hrr_nowait{0},
    hrr_waited{0}, hrr_rb_ns{0}, hrr_rb_count{0}, hrr_demotions{0}, hrr_import_fail{0},
    hrr_faults_all{0};
inline std::atomic<uint64_t> rb_batch_submits{0};
inline std::atomic<uint64_t> rb_batch_saved{0};
inline std::atomic<uint64_t> readback_finish{0};  ///< downloads that flushed the current work
inline std::atomic<uint64_t> readback_finish_asked{0}; ///< ... for bytes the guest itself asked
inline std::atomic<uint64_t> heap_guard_builds{0}; ///< readbacks into the BPE heap
inline std::atomic<uint64_t> heap_guard_stale_hits{0}; ///< writes the previous guard vouched for
inline std::atomic<uint64_t> upload_deferred{0};        ///< first uploads put off to a later frame
inline std::atomic<uint64_t> upload_deferred_bytes{0};
inline std::atomic<uint64_t> upload_zero_filled{0};     ///< ... images filled with zeros meanwhile
inline std::atomic<uint64_t> heap_guard_ns{0};        ///< time of the guard builds (free-tree walks)
inline std::atomic<uint64_t> heap_guard_worst_ns{0};
inline std::atomic<uint64_t> heap_guard_worst_blocks{0}; ///< free blocks of the slowest build
inline std::atomic<uint64_t> heap_guard_worst_adds{0};   ///< ... and the visits it made
inline std::atomic<uint64_t> heap_guard_repeats{0};      ///< nodes and ring members seen again, skipped
inline std::atomic<uint64_t> readback_copies{0};          ///< ranges downloaded
inline std::atomic<uint64_t> readback_copies_changed{0};  ///< ... whose bytes differed from memory
inline std::atomic<uint64_t> readback_bytes{0};
inline std::atomic<uint64_t> readback_bytes_changed{0};
/// sotc: BufferCache::ReleaseMemory's GPU sync (Finish, or Flush with SOTC_UNBIND_GPU_WAIT=1):
/// calls and time (SOTC_FPS only).
inline std::atomic<uint64_t> release_finish_count{0}, release_finish_ns{0};
/// sotc: SOTC_UNBIND_DEFER: unmaps whose unbind was deferred, and deferred unbinds that a bind of their blocks had to
/// wait for (Scheduler::WaitSubmitted on the draw thread), with that wait.
inline std::atomic<uint64_t> unbind_deferred{0}, unbind_forced{0}, unbind_forced_ns{0};
/// sotc: time the guest thread waits in BufferCache::DropGpuModified. The default (SOTC_DROP_ASYNC
/// unset) waits for the command processor; SOTC_DROP_ASYNC=1 (experimental, off) orders the drop
/// with the stream instead, and this stays 0. SOTC_FPS only.
inline std::atomic<uint64_t> drop_gpu_wait_ns{0};
/// sotc: SOTC_DROP_FAST: unmaps dropped on the guest thread, and those that still waited
/// (GPU-written pages in the range). Totals.
inline std::atomic<uint64_t> drop_fast{0};
/// sotc: SOTC_DROP_NATIVE: unmaps of GPU-written ranges forgotten on the guest thread instead of waited for.
inline std::atomic<uint64_t> drop_native{0};
/// sotc: SOTC_ASYNC_UPLOAD, totals: uploads copied by the copy threads and their bytes, the time
/// submits waited for them, the guest writes / unmaps / frees that waited for a job (the write
/// fence) and that time, and the large uploads kept on the draw thread (hot pages, no backing).
inline std::atomic<uint64_t> async_upload_jobs{0};
inline std::atomic<uint64_t> async_upload_bytes{0};
inline std::atomic<uint64_t> async_upload_submit_ns{0};
inline std::atomic<uint64_t> async_upload_fence_waits{0};
inline std::atomic<uint64_t> async_upload_fence_ns{0};
inline std::atomic<uint64_t> async_upload_kept{0};
/// sotc: SOTC_FENCE_PIECES (reset by each SOTC_FPS report): fence waits for single pieces and
/// their time, pieces the waiting thread copied itself, and guest read faults let through
/// without waiting while copies were pending.
inline std::atomic<uint64_t> fence_piece_waits{0}, fence_piece_ns{0};
inline std::atomic<uint64_t> fence_pieces_helped{0}, fence_read_skips{0};
/// sotc: SOTC_HOT_PAGES, totals: pages made sticky (writable for their region's lifetime) and the
/// ones refused at the cap.
inline std::atomic<uint64_t> hot_pages_marks{0};
inline std::atomic<uint64_t> hot_pages_cap_hits{0};
/// sotc: SOTC_HOT_PAGES adaptive cap (region_manager.h HotPages): times the cap grew / shrank.
inline std::atomic<uint64_t> hot_pages_cap_grows{0}, hot_pages_cap_shrinks{0};
inline std::atomic<uint64_t> async_texture_uploads{0}; ///< SOTC_ASYNC_TEXTURE_UPLOAD, total
inline std::atomic<uint64_t> image_pool_allocs{0}; ///< SOTC_IMAGE_POOL_MB: images from the pool
/// sotc: SOTC_CP_FLIP_PACE: command processor time spent waiting for a free presentation frame
/// before queueing a flip (since the last SOTC_FPS report).
inline std::atomic<uint64_t> cp_flip_pace_ns{0};
/// sotc: SOTC_STALL_DETAIL: the same pacing time, never reset (the report exchanges cp_flip_pace_ns).
inline std::atomic<uint64_t> cp_flip_pace_all_ns{0};
/// sotc: SOTC_HOST_IMPORT, totals: uploads copied by the GPU from imported guest memory, their
/// bytes and wait time, uploads that went back to the CPU path, chunks the driver refused, and
/// the chunks imported now (64 MiB each).
inline std::atomic<uint64_t> host_import_uploads{0};
inline std::atomic<uint64_t> host_import_bytes{0};
inline std::atomic<uint64_t> host_import_ns{0};
inline std::atomic<uint64_t> host_import_fallbacks{0};
inline std::atomic<uint64_t> host_import_refused{0};
inline std::atomic<uint64_t> host_import_live_chunks{0};
inline std::atomic<uint64_t> drop_slow{0};
/// sotc: SOTC_DROP_FAST guard: unmaps that took the waiting path because the range overlaps a
/// command buffer the command processor has not finished reading (Liverpool::CmdRangeInUse).
inline std::atomic<uint64_t> drop_fast_guarded{0};
/// sotc: SOTC_DROP_FAST guard: submits whose nested indirect buffer walk hit a cap (ranges, packets
/// or depth). Their ranges are incomplete, so unmaps wait while one of them is in flight.
inline std::atomic<uint64_t> drop_fast_overflows{0};
/// sotc: SOTC_DROP_FAST guard: nested indirect buffers the walk skipped because their memory is
/// not mapped (Liverpool::CollectNestedIbs).
inline std::atomic<uint64_t> drop_fast_unmapped_ibs{0};
/// sotc: SOTC_DROP_FAST guard: the nested indirect buffer walk stopped before the end of a command
/// buffer (a non-packet or type 0/1 dword, a packet running past the span) or skipped an unmapped
/// indirect buffer. Those submits are marked overflowed, so unmaps wait while they are in flight.
inline std::atomic<uint64_t> drop_fast_walk_gaps{0};
/// sotc: SOTC_DROP_FAST guard: indirect buffers the parser registered while it ran (CmdRangeGuard)
/// that the submit-time walk had not registered. A non-zero value means the walk missed a buffer.
inline std::atomic<uint64_t> drop_fast_late_ibs{0};
/// sotc: submits accepted by the GPU command path (SubmitGfx and SubmitAsc), a running count. The
/// drop ring stores it per fast drop, so a bad packet can say how many submits ago the drop was.
inline std::atomic<uint64_t> submit_tick{0};
/// sotc: SOTC_DROP_FAST diagnostics: the last kDropRing fast-dropped ranges, so a bad PM4 packet
/// can tell whether it was read from memory the guest had already unmapped (FastDropAge).
/// Each slot is a seqlock, so a reader never mixes the bounds of two drops.
inline constexpr uint32_t kDropRing = 256;
struct DropSlot {
    std::atomic<uint32_t> seq{0}; ///< odd while NoteFastDrop writes the slot
    std::atomic<uint64_t> lo{0};
    std::atomic<uint64_t> hi{0};
    std::atomic<uint64_t> tick{0}; ///< submit_tick when the range was dropped
};
inline DropSlot drop_ring[kDropRing]{};
inline std::atomic<uint32_t> drop_ring_head{0};
inline void NoteFastDrop(uint64_t addr, uint64_t size) noexcept {
    const uint64_t end = size > ~uint64_t{0} - addr ? ~uint64_t{0} : addr + size; // saturates
    const uint64_t tick = submit_tick.load(std::memory_order_relaxed);
    const uint32_t i = drop_ring_head.fetch_add(1, std::memory_order_relaxed) % kDropRing;
    DropSlot& s = drop_ring[i];
    const uint32_t q = s.seq.load(std::memory_order_relaxed);
    s.seq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.lo.store(addr, std::memory_order_relaxed);
    s.hi.store(end, std::memory_order_relaxed);
    s.tick.store(tick, std::memory_order_relaxed);
    s.seq.store(q + 2, std::memory_order_release);
}
/// How many fast drops ago `addr` was inside a dropped range (1 = the newest); 0 when it is not in
/// any of the last kDropRing. When found and tick_out is set, *tick_out is the submit_tick of that
/// drop.
inline uint32_t FastDropAge(uint64_t addr, uint64_t* tick_out = nullptr) noexcept {
    const uint32_t head = drop_ring_head.load(std::memory_order_relaxed);
    for (uint32_t k = 1; k <= kDropRing && k <= head; ++k) {
        const DropSlot& s = drop_ring[(head - k) % kDropRing];
        for (int retry = 0; retry < 8; ++retry) {
            const uint32_t s1 = s.seq.load(std::memory_order_acquire);
            if (s1 & 1) {
                continue; // being written
            }
            const uint64_t lo = s.lo.load(std::memory_order_relaxed);
            const uint64_t hi = s.hi.load(std::memory_order_relaxed);
            const uint64_t tick = s.tick.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) != s1) {
                continue; // overwritten while read
            }
            if (addr >= lo && addr < hi) {
                if (tick_out) {
                    *tick_out = tick;
                }
                return k;
            }
            break;
        }
    }
    return 0;
}
/// SOTC_RB_NARROW: readback window (KiB, 0 = off), GPU-modified bytes of the 512 KiB window left
/// outside the narrow one (not downloaded nor unprotected), live hot readbacks (gauge).
inline std::atomic<uint64_t> rb_narrow_kib{0}, rb_narrow_kept_bytes{0}, rb_hot_entries{0};
/// sotc: readbacks with a streaming window (SOTC_RB_STREAM, BufferCache::ReadMemory)
inline std::atomic<uint64_t> rb_stream_windows{0};
inline std::atomic<uint64_t> uploads{0};        ///< SynchronizeMemory copies (CPU data to GPU)
/// SOTC_XFER_QUEUE: uploads copied on the transfer queue instead of the graphics queue.
inline std::atomic<uint64_t> xfer_copies{0};
/// DMA sweeps (SynchronizeDmaBuffers): count, resident ranges visited, sweeps that uploaded.
inline std::atomic<uint64_t> dma_sweeps{0}, dma_sweep_ranges{0}, dma_sweep_uploading{0};
/// ... merged runs visited, and time (ns) in all sweeps and in the ones that uploaded nothing.
inline std::atomic<uint64_t> dma_sweep_runs{0}, dma_sweep_ns{0}, dma_sweep_idle_ns{0};
inline std::atomic<uint64_t> upload_bytes{0};
/// sotc: SOTC_HASH_SMALL=<bytes> (default 0 = off, read once at static initialisation). A read-only
/// bind of at most that many bytes whose pages the tracker reports CPU-dirty is hashed (XXH3 of
/// the guest bytes) against the bytes last uploaded for the same bind range: equal bytes skip the
/// upload (BufferCache::SyncSmallByHash). Pages stay write-protected: a guest write still faults.
inline const uint64_t kHashSmall = [] {
    const char* v = std::getenv("SOTC_HASH_SMALL");
    return v ? std::strtoull(v, nullptr, 0) : uint64_t{0};
}();
/// sotc: hashed binds whose bytes were unchanged (upload skipped), hashed binds uploaded, and the
/// bytes of the skipped uploads. Printed with the SOTC_FPS stats (videoout/driver.cpp).
inline std::atomic<uint64_t> hash_small_hits{0};
inline std::atomic<uint64_t> hash_small_uploads{0};
inline std::atomic<uint64_t> hash_small_bytes{0};
inline std::atomic<uint64_t> render_breaks{0};  ///< EndRendering that closed a render pass
/// sotc: SOTC_PASS_SEC passes recorded on a secondary command buffer, and passes over the draw
/// threshold that stayed on the primary (a block the secondary cannot hold, or no pass format)
inline std::atomic<uint64_t> passes_secondary{0};
inline std::atomic<uint64_t> passes_fallback{0};
inline std::atomic<uint64_t> fault_writes{0};       ///< guest write faults on tracked pages
inline std::atomic<uint64_t> fault_walker{0};       ///< SRT walker loads emulated after a fault
inline std::atomic<uint64_t> fault_gpu_reads{0};    ///< other read faults on the command processor
/// sotc: SOTC_FAULT_GRANULE=<bytes> (16384 or 65536; unset or anything else = 4096, the page). A
/// guest write fault also invalidates the other pages of its aligned granule, so writes that
/// follow in the same granule do not fault again. Read once at static initialisation.
inline const uint64_t kFaultGranule = [] {
    const char* v = std::getenv("SOTC_FAULT_GRANULE");
    const uint64_t g = v ? std::strtoull(v, nullptr, 10) : 4096;
    return (g == 16384 || g == 65536) ? g : uint64_t{4096};
}();
/// sotc: pages invalidated beyond the faulting one by kFaultGranule (SOTC_CMD_STATS).
inline std::atomic<uint64_t> fault_granule_pages{0};
/// sotc: SOTC_FAULT_GRANULE pages not uploaded because the guest left them unchanged.
inline std::atomic<uint64_t> fault_granule_skipped{0};
inline std::atomic<uint64_t> commands{0}; ///< draws and dispatches recorded (monotonic, CP only)
/// Change of a monotonic single-writer counter since the last call (`seen` keeps the last value).
inline uint64_t TakeDelta(const std::atomic<uint64_t>& counter, uint64_t& seen) {
    const uint64_t now = counter.load(std::memory_order_relaxed);
    const uint64_t delta = now - seen;
    seen = now;
    return delta;
}
/// Command processor waits for the draw thread (Liverpool::IsBackThreaded).
inline std::atomic<uint64_t> cp_waits{0};
/// Busy time of the two command processor threads (SOTC_FPS): the draw thread minus the time it
/// waits for work, the parser as the time inside the command buffer coroutines (not the polling
/// loop around them, which spins while a queue waits). sotc: only resumes that made progress are
/// timed (one clock read each); resumes that only re-test a wait are cp_spin_ns.
inline std::atomic<uint64_t> draw_idle_ns{0};
inline std::atomic<uint64_t> cp_parse_ns{0};
inline uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
} ///< draws and dispatches recorded
/// sotc: SOTC_WRITE_AHEAD_ADAPT=1 (off by default; unset = the fixed write-ahead of
/// SOTC_WRITE_AHEAD_PAGES, 15 pages). A guest thread that keeps taking write faults right after
/// the extent its previous fault unprotected (a sequential writer: memcpy of a streamed block)
/// doubles its write-ahead window on every such fault, from 16 pages up to
/// SOTC_WRITE_AHEAD_MAX_PAGES (default 512 = 2 MiB), in its direction of travel; a fault that is
/// not adjacent, or later than SOTC_WRITE_AHEAD_ADAPT_US (default 1000) after the previous one
/// finished, resets it. The pages it marks are snapshot-hashed first (like the fault granule), so
/// those the guest never writes are not uploaded (BufferCache::DropUnchangedGranulePages).
inline const bool kWriteAheadAdapt = [] {
    const char* v = std::getenv("SOTC_WRITE_AHEAD_ADAPT");
    return v && v[0] == '1';
}();
inline const uint64_t kWriteAheadAdaptUs = [] {
    const char* v = std::getenv("SOTC_WRITE_AHEAD_ADAPT_US");
    const uint64_t us = v ? std::strtoull(v, nullptr, 10) : 1000;
    return us < 10 ? uint64_t{10} : (us > 1000000 ? uint64_t{1000000} : us);
}();
inline const uint32_t kWriteAheadMaxPages = [] {
    const char* v = std::getenv("SOTC_WRITE_AHEAD_MAX_PAGES");
    const uint64_t p = v ? std::strtoull(v, nullptr, 10) : 512;
    return static_cast<uint32_t>(p < 16 ? 16 : (p > 4096 ? 4096 : p));
}();
/// sotc: guest write fault accounting (SOTC_FPS; the "SOTC FAULTS" line in videoout/driver.cpp and
/// "faults" in the SOTCSTALL lines). A fault is timed from the entry of bbgpu_handle_fault to its
/// exit; inside it the time waiting for a region mutex, for the async upload fence and in the host
/// protect call is kept apart (the texture cache's own lock is not split out). Sequential means: the
/// same thread, right after the extent its previous fault unprotected (see FaultObserve).
namespace FaultStat {
inline std::atomic<uint64_t> count{0}, seq{0}, total_ns{0}, mutex_ns{0}, fence_ns{0}, protect_ns{0};
inline std::atomic<uint64_t> flush_ns{0}; ///< sotc: time in the readback of MemoryTracker::InvalidateRegion
inline std::atomic<uint64_t> ahead_pages{0};     ///< pages the write-ahead marked
inline std::atomic<uint64_t> ahead_unchanged{0}; ///< of those snapshotted, left unchanged at upload
inline std::atomic<uint64_t> all_count{0}, all_ns{0}; ///< never reset (FrameAccount)
inline constexpr unsigned kBuckets = 256;
inline std::atomic<uint64_t> hist[kBuckets]{}; ///< 4 buckets per power of two of the latency in ns
inline unsigned Bucket(uint64_t ns) {
    if (ns < 4) {
        return static_cast<unsigned>(ns);
    }
    const unsigned msb = 63 - static_cast<unsigned>(__builtin_clzll(ns));
    return (msb - 1) * 4 + static_cast<unsigned>((ns >> (msb - 2)) & 3);
}
/// Upper edge (ns) of a bucket.
inline uint64_t BucketUpper(unsigned idx) {
    if (idx < 4) {
        return idx + 1;
    }
    const unsigned msb = idx / 4 + 1;
    return static_cast<uint64_t>(5 + idx % 4) << (msb - 2);
}
struct Tl {
    bool active = false;   ///< inside bbgpu_handle_fault with SOTC_FPS on
    bool counted = false;  ///< the fault was a handled guest write fault
    bool sequential = false;
    uint64_t t0 = 0, mutex_ns = 0, fence_ns = 0, protect_ns = 0, flush_ns = 0;
    uint64_t cur_page = 0, last_page = 0, last_lo = 0, last_hi = 0, last_end_ns = 0;
    uint32_t window = 16; ///< pages, kWriteAheadAdapt
    int dir = 0;          ///< +1 forward, -1 backward, 0 none
};
inline thread_local Tl tl;
/// sotc: where a handled guest write fault spends its time, in stages that do not overlap (the
/// "SOTC FAULT SPLIT" line). Each stage keeps its own time: the mutex and host-protect time
/// measured inside it (tl.mutex_ns, tl.protect_ns) and the time of stages nested in it are taken
/// out, and those two are reported on their own.
///  entry    bbgpu_handle_fault entry up to the page manager's write-fault handler (dispatch,
///           without the async-upload fence wait, which is tl.fence_ns)
///  inval    Rasterizer::InvalidateMemoryOnGuestFault of the faulting page: the texture cache and
///           the buffer cache (readback not split out, see tl.flush_ns)
///  tc       the texture cache part of inval, when the rasterizer marks it (StageScope kStTc)
///  granule  SOTC_FAULT_GRANULE: the neighbouring pages' checks and invalidation
///  granhash ... of which snapshotting their bytes (BufferCache::NoteGranulePage)
///  ahead    write-ahead (fixed or SOTC_WRITE_AHEAD_ADAPT)
///  flush    SOTC_PROTECT_COALESCE: applying the collected unprotects
///  repair   RepairStuckWriteFault
enum Stage : unsigned { kStEntry, kStInval, kStTc, kStGranule, kStGranHash, kStAhead, kStFlush,
                        kStRepair, kStCount };
inline constexpr const char* kStageNames[kStCount] = {"entry",   "inval", "tc",    "granule",
                                                      "granhash", "ahead", "flush", "repair"};
inline std::atomic<uint64_t> stage_total_ns[kStCount]{};
struct StageTl {
    uint64_t ns[kStCount]{};
    uint64_t sum = 0; ///< all stage time kept so far (the nested stages of a scope are its delta)
};
inline thread_local StageTl stl;
/// Times a stage of the fault being handled on this thread (see above).
struct StageScope {
    unsigned stage;
    uint64_t start = 0, mutex0 = 0, protect0 = 0, sum0 = 0;
    bool on;
    explicit StageScope(unsigned s) : stage{s}, on{kFpsTimers && tl.active} { // sotc: SOTC_FPS_LEAN
        if (on) {
            start = NowNs();
            mutex0 = tl.mutex_ns;
            protect0 = tl.protect_ns;
            sum0 = stl.sum;
        }
    }
    ~StageScope() {
        if (!on) {
            return;
        }
        const uint64_t total = NowNs() - start;
        const uint64_t inner = (tl.mutex_ns - mutex0) + (tl.protect_ns - protect0) + (stl.sum - sum0);
        const uint64_t own = total > inner ? total - inner : 0;
        stl.ns[stage] += own;
        stl.sum += own;
    }
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;
};
/// The write-fault handler of the page manager is reached: everything since Begin that was not the
/// fence wait was dispatch ("entry").
inline void NoteHandlerEntry() {
    if (kFpsTimers && tl.active) { // sotc: SOTC_FPS_LEAN: clock reads
        const uint64_t spent = NowNs() - tl.t0;
        const uint64_t own = spent > tl.fence_ns ? spent - tl.fence_ns : 0;
        stl.ns[kStEntry] = own;
        stl.sum += own;
    }
}
/// sotc: SOTC_FAULT_OSCOST=1 (SOTC_FPS): what the operating system adds to a write fault around the
/// handler, measured once on a private page (page_manager.cpp MeasureFaultOsCost): the time from the
/// faulting store to the entry of bbgpu_handle_fault, and from the handler's exit to the store
/// completing. 0 = not measured.
inline std::atomic<uint64_t> os_fault_in_ns{0}, os_fault_out_ns{0};
/// Adds the scope's duration to a field of tl while a fault is being timed.
struct Scope {
    uint64_t Tl::*field;
    uint64_t start = 0;
    bool on;
    explicit Scope(uint64_t Tl::*f) : field{f}, on{kFpsTimers && tl.active} { // sotc: SOTC_FPS_LEAN
        if (on) {
            start = NowNs();
        }
    }
    ~Scope() {
        if (on) {
            tl.*field += NowNs() - start;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline void Begin() {
    tl.active = true;
    tl.counted = false;
    tl.mutex_ns = tl.fence_ns = tl.protect_ns = tl.flush_ns = 0;
    for (unsigned i = 0; i < kStCount; ++i) {
        stl.ns[i] = 0;
    }
    stl.sum = 0;
    // sotc: SOTC_FPS_LEAN: the fault still counts without kFpsTimers, only the clock is skipped.
    tl.t0 = kFpsTimers ? NowNs() : 0;
}
inline void End() {
    if (!tl.active) {
        return;
    }
    tl.active = false;
    if (!tl.counted) {
        return;
    }
    count.fetch_add(1, std::memory_order_relaxed);
    all_count.fetch_add(1, std::memory_order_relaxed);
    if (tl.sequential) {
        seq.fetch_add(1, std::memory_order_relaxed);
    }
    if (!kFpsTimers) { // sotc: SOTC_FPS_LEAN: the time sums and the histogram need the clock
        return;
    }
    const uint64_t ns = NowNs() - tl.t0;
    total_ns.fetch_add(ns, std::memory_order_relaxed);
    all_ns.fetch_add(ns, std::memory_order_relaxed);
    mutex_ns.fetch_add(tl.mutex_ns, std::memory_order_relaxed);
    fence_ns.fetch_add(tl.fence_ns, std::memory_order_relaxed);
    protect_ns.fetch_add(tl.protect_ns, std::memory_order_relaxed);
    flush_ns.fetch_add(tl.flush_ns, std::memory_order_relaxed);
    for (unsigned i = 0; i < kStCount; ++i) {
        stage_total_ns[i].fetch_add(stl.ns[i], std::memory_order_relaxed);
    }
    hist[Bucket(ns)].fetch_add(1, std::memory_order_relaxed);
}
/// What the write-ahead does for a guest write fault on the page at `page` (4 KiB aligned).
struct Plan {
    bool sequential = false;
    int dir = 0;
    uint32_t window = 16; ///< pages
};
/// Classifies the fault against this thread's previous one: sequential when it comes within
/// kWriteAheadAdaptUs of the previous fault's end, in the same direction as the run so far, at
/// most 4 pages beyond the extent the previous fault unprotected (or on a page of that extent that
/// was protected again). Only called with SOTC_FPS or SOTC_WRITE_AHEAD_ADAPT on.
inline Plan Observe(uint64_t page) {
    constexpr uint64_t kPage = 4096, kSlack = 4 * kPage;
    Tl& t = tl;
    Plan plan;
    t.cur_page = page;
    // sotc: the clock is read only for SOTC_WRITE_AHEAD_ADAPT: the plan is used only with it.
    if (kWriteAheadAdapt && t.last_hi != 0 && NowNs() - t.last_end_ns <= kWriteAheadAdaptUs * 1000) {
        int dir = 0;
        if (page > t.last_page && page <= t.last_hi + kSlack) {
            dir = 1;
        } else if (page < t.last_page && page + kSlack >= t.last_lo) {
            dir = -1;
        }
        if (dir != 0 && (t.dir == 0 || t.dir == dir)) {
            plan.sequential = true;
            plan.dir = dir;
            const uint64_t doubled = uint64_t{t.window} * 2;
            plan.window = static_cast<uint32_t>(doubled < kWriteAheadMaxPages
                                                    ? doubled
                                                    : kWriteAheadMaxPages);
        }
    }
    t.sequential = plan.sequential;
    t.dir = plan.dir;
    t.window = plan.window;
    return plan;
}
/// Records the extent [lo, hi) the fault unprotected (granule, write-ahead) and counts it.
inline void Done(uint64_t lo, uint64_t hi) {
    Tl& t = tl;
    t.last_page = t.cur_page;
    t.last_lo = lo < t.cur_page ? lo : t.cur_page;
    t.last_hi = hi > t.cur_page + 4096 ? hi : t.cur_page + 4096;
    t.last_end_ns = kWriteAheadAdapt ? NowNs() : 0; // sotc: read only for SOTC_WRITE_AHEAD_ADAPT
    t.counted = true;
}
} // namespace FaultStat

/// sotc: SOTC_DRAW_SPLIT=1 (with SOTC_FPS): where the draw thread's time goes, the "SOTC DRAW
/// SPLIT" lines (videoout/driver.cpp; step 0 of docs/sotc/PLANO-THREAD-DESENHO.md). "draw busy"
/// counts every wait inside an operation (flips, GPU waits, readbacks) as busy; these counters
/// separate the operations, the binds and the waits, in TSC ticks (one rdtsc per edge), and the
/// printer reads the CPU time of the draw, command processor and recorder threads. Counting only:
/// nothing else changes.
inline const bool kDrawSplit = std::getenv("SOTC_DRAW_SPLIT") != nullptr;
enum SplitPart {
    SplitDraw, SplitIndirect, SplitDispatch, SplitFunc, SplitCommands, // draw thread operations
    SplitBind, SplitBindBuffers, SplitBindTextures, SplitBindDma,      // inside BindResources
    SplitTexRefresh, SplitTexHash, SplitTexStaging, SplitTexCopy,      // TextureCache::RefreshImage
    SplitTexDetile, SplitTexRecord,
    SplitWaitGpu, SplitWaitSubmit, SplitWaitRecorder, SplitWaitFrame,  // draw thread blocked
    SplitIdle, SplitIdleSpin,                                          // draw thread out of work
    SplitRbQueue, SplitRbSubmit, SplitRbGpu, SplitRbFinish,            // guest readback waits
    SplitRbCount, ///< guest readback waits (a count, not ticks)
    /// Texture copies from guest memory on the draw thread (counts, not ticks): bytes, copies,
    /// copies of 2 MiB and more (split across the copy threads), process page faults meanwhile.
    SplitTexBytes, SplitTexCopies, SplitTexBig, SplitTexFaults,
    SplitParts
};
inline std::atomic<uint64_t> split_ticks[SplitParts]{};
/// Deepest draw thread queue (back_head - back_tail at a publish) since the last flip.
inline std::atomic<uint64_t> split_queue_max{0};
/// Set by Liverpool::BackLoop: the waits count only on the draw thread.
inline thread_local bool t_draw_thread = false;
inline uint64_t Ticks() {
    return __builtin_ia32_rdtsc();
}
struct SplitTimer {
    SplitPart part;
    bool on;
    uint64_t start;
    // sotc: with SOTC_DRAW_SPLIT off the constructor is one test of kDrawSplit and the destructor
    // one test of `on`; the counter update is out of line (RecordSplit). Inline so that the
    // draw thread's per-op timers do not cost a call each.
    [[gnu::always_inline]] explicit SplitTimer(SplitPart p, bool draw_thread_only = true)
        : part{p}, on{kDrawSplit && (!draw_thread_only || t_draw_thread)}, start{on ? Ticks() : 0} {}
    [[gnu::always_inline]] ~SplitTimer() {
        if (on) {
            RecordSplit();
        }
    }

private:
    __attribute__((noinline)) void RecordSplit() const {
        split_ticks[part].fetch_add(Ticks() - start, std::memory_order_relaxed);
    }
};
enum SplitThread { SplitThreadDraw, SplitThreadCp, SplitThreadRecorder, SplitThreads };
/// The calling thread is that one (its CPU time is read by SplitThreadCycles); sotc_stats.cpp.
void RegisterSplitThread(SplitThread which);
/// CPU time of a registered thread in TSC ticks (QueryThreadCycleTime), 0 if not registered.
uint64_t SplitThreadCycles(SplitThread which);
/// Page faults of the whole process so far (GetProcessMemoryInfo), 0 where unknown.
uint64_t SplitProcessFaults();
/// A texture copy of `bytes` from guest memory: its wall time goes to SplitTexCopy and the counts
/// to SplitTexBytes..SplitTexFaults (draw thread only, SOTC_DRAW_SPLIT).
struct SplitTexCopyScope {
    bool on;
    uint64_t start, faults;
    explicit SplitTexCopyScope(uint64_t bytes) : on{kDrawSplit && t_draw_thread} {
        start = on ? Ticks() : 0;
        faults = on ? SplitProcessFaults() : 0;
        if (on) {
            split_ticks[SplitTexBytes].fetch_add(bytes, std::memory_order_relaxed);
            split_ticks[SplitTexCopies].fetch_add(1, std::memory_order_relaxed);
            if (bytes >= (2u << 20)) {
                split_ticks[SplitTexBig].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ~SplitTexCopyScope() {
        if (on) {
            split_ticks[SplitTexCopy].fetch_add(Ticks() - start, std::memory_order_relaxed);
            split_ticks[SplitTexFaults].fetch_add(SplitProcessFaults() - faults,
                                                  std::memory_order_relaxed);
        }
    }
};
/// SOTC_CMD_STATS=1: draws, indirect draws (arguments GPU-written / CPU-side / CPU-side and
/// empty), dispatches, indirect dispatches.
inline std::atomic<uint64_t> cmd_kinds[7]{}; // [6]: direct draws with 0 indices or 0 instances
/// sotc: IT_SET_PREDICATION packets by predication op (0 clear, 1 zpass, 2 primcount, 3 bool64,
/// other), SOTC_CMD_STATS only; printed as the "SOTC predication" line (videoout/driver.cpp).
inline std::atomic<uint64_t> pred_ops[5]{};
/// SOTC_CMD_STATS: command blocks the recording thread ran, per CommandRecorder::BlockCategory
/// (vk_recorder.cpp counts them, the SOTC_FPS printer takes them per window).
inline std::atomic<uint64_t> recorder_blocks[16]{};
inline std::atomic<uint64_t> early_flushes{0}; ///< submits right after a write to a read-back page
/// Suspects of long frames, printed for each frame over SOTC_STALL_MS (SOTC_FRAME_LOG runs):
/// pipelines created, shaders recompiled and images created during the frame, and their time.
inline std::atomic<uint64_t> pipelines_new{0}, pipeline_ns{0};
inline std::atomic<uint64_t> shaders_compiled{0}, shader_ns{0};
inline std::atomic<uint64_t> images_new{0}, image_ns{0};
/// Parts of an image creation: format query, memory allocation (and how many got memory of
/// their own instead of a block shared with others).
inline std::atomic<uint64_t> image_query_ns{0}, image_alloc_ns{0}, image_dedicated{0};
/// Video memory: usage the driver reports (texture cache GC), budget the GC works with, and the
/// sparse buffer arena memory (never released) as allocations and bytes.
inline std::atomic<uint64_t> vram_used{0}, vram_budget{0}, arena_allocs{0}, arena_bytes{0};
/// Texture cache collector: ticks (one per submit with a frame's work), images freed and those
/// written back to memory first, and the images in the cache with their (guest) size.
inline std::atomic<uint64_t> gc_ticks{0}, gc_freed{0}, gc_downloads{0};
inline std::atomic<uint64_t> cached_images{0}, cached_image_bytes{0};
inline std::atomic<uint64_t> vram_emergency{0}; ///< emergency texture collections (allocation failed)
inline std::atomic<uint64_t> write_fault_gpu_pending{0}; ///< write-fault readbacks left protected
inline std::atomic<uint64_t> write_fault_stale_gpu{0};   ///< ... that cleared a stale GPU page bit
inline std::atomic<uint64_t> arena_evicted{0};  ///< arena bytes given back by MaybeEvictCold
inline std::atomic<uint64_t> arena_rebound{0};  ///< ... bound again afterwards
inline std::atomic<uint64_t> arena_pinned{0};   ///< ... faulted back in by shaders (kept since)
inline std::atomic<uint64_t> arena_evict_mismatch{0}; ///< read back unequal to guest memory: kept
inline std::atomic<uint64_t> arena_evict_runs{0};
inline std::atomic<uint64_t> arena_evict_ns{0}; ///< time in MaybeEvictCold (scan, GPU wait, unbind)
inline std::atomic<uint64_t> arena_tick_waits{0}; ///< SOTC_ARENA_TICK_FREE: waits for freed arena runs
/// sotc: SOTC_HRR=1 (buffer_cache/hrr.h): arena bytes and chunks in host-visible memory, now.
inline std::atomic<uint64_t> hrr_host_bytes{0}, hrr_host_allocs{0};
/// sotc: SOTC_HRR=1 (BufferCache::MigrateHostResident): granules moved from device to host
/// memory, and their bytes.
/// hrr_migrate_failed: migrations a check kept in device memory (retried, see MigrateHostResident).
inline std::atomic<uint64_t> hrr_migrated{0}, hrr_migrated_bytes{0}, hrr_migrate_failed{0};
/// sotc: SOTC_HRR=1 direct readbacks (BufferCache::FinishAsyncReadback): readbacks that read their
/// bytes from host-visible arena chunks instead of a staging copy, and those bytes.
inline std::atomic<uint64_t> hrr_direct_readbacks{0}, hrr_direct_bytes{0};
/// sotc: SOTC_ADAPTIVE_LOAD (sotc_adaptive.h), per report (SOTC_FPS only): flips seen and those
/// while relieved, engagements, far cascades moved instead of drawn because of the governor,
/// periodic submits left out (approximate: base-interval submits a longer window replaced),
/// texture collector passes and cold arena checks that waited.
inline std::atomic<uint64_t> adaptive_flips{0}, adaptive_relieved_flips{0}, adaptive_engaged{0};
inline std::atomic<uint64_t> adaptive_amortised{0}, adaptive_flushes_saved{0};
inline std::atomic<uint64_t> adaptive_gc_deferred{0}, adaptive_evict_deferred{0};
// sotc: compute pipelines built by pipeline workers (SOTC_ASYNC_COMPUTE), totals.
inline std::atomic<uint64_t> async_compute_built{0};
inline std::atomic<uint64_t> async_compute_build_ns{0}; ///< worker time building them
inline std::atomic<uint64_t> async_compute_waits{0};    ///< binds the recording thread waited for
inline std::atomic<uint64_t> async_compute_wait_ns{0};
inline std::atomic<uint64_t> async_compute_self_built{0}; ///< built by the recording thread itself
inline std::atomic<uint64_t> async_translate_stale{0}; ///< permutations whose tables changed
// sotc: graphics shader stages translated by pipeline workers (SOTC_ASYNC_TRANSLATE), totals.
inline std::atomic<uint64_t> async_translate_built{0};
inline std::atomic<uint64_t> async_translate_ns{0};    ///< worker time translating them
inline std::atomic<uint64_t> async_translate_skips{0}; ///< draws skipped while waiting for one
// sotc: SOTC_ASYNC_COMPUTE_XLATE, totals: compute shaders queued ahead of their dispatch, the
// dispatches that waited for one (and the wait), and those taken back before a worker started.
inline std::atomic<uint64_t> compute_prefetch_queued{0};
inline std::atomic<uint64_t> compute_prefetch_scans{0};
inline std::atomic<uint64_t> compute_prefetch_waits{0};
inline std::atomic<uint64_t> compute_prefetch_wait_ns{0};
inline std::atomic<uint64_t> compute_prefetch_taken_back{0};
inline std::atomic<uint64_t> compute_prefetch_timeouts{0};
// sotc: SOTC_ASYNC_IMAGE_CREATE, totals (VideoCore::ImagePrefetcher): creations the command
// processor queued and dropped (cap, budget, limits), images the workers made (and their time)
// or could not make within the budget, images Image::Image took ready, took after waiting for a
// worker (and the wait), took back still queued and created itself, created itself without any
// match, and made images nobody took (destroyed).
inline std::atomic<uint64_t> async_image_requested{0}, async_image_skipped{0};
inline std::atomic<uint64_t> async_image_built{0}, async_image_build_ns{0}, async_image_failed{0};
inline std::atomic<uint64_t> async_image_taken{0}, async_image_waits{0}, async_image_wait_ns{0};
inline std::atomic<uint64_t> async_image_taken_back{0}, async_image_sync{0};
inline std::atomic<uint64_t> async_image_expired{0};
/// sotc: SRT walker loads that faulted (null or unmapped table pointer) and read zero, total.
inline std::atomic<uint64_t> srt_walker_faults{0};
/// sotc: Phi operands of SRT walker offsets: resolved to their one common value, and fallbacks
/// (operands differ, or a cycle through Phis). Counted when the shader is translated.
inline std::atomic<uint64_t> phi_resolved{0}, phi_fallback{0};
inline std::atomic<uint64_t> image_bytes{0};    ///< live texture cache and host pass images
inline std::atomic<uint64_t> image_count{0};
inline std::atomic<uint64_t> gc_kept_tiled{0};  ///< GC candidates kept: GPU-written tiled images
inline std::atomic<uint64_t> vma_block_bytes{0};      ///< VMA device-local blocks
inline std::atomic<uint64_t> vma_allocation_bytes{0}; ///< ... and the allocations inside them
/// Image census (TextureCache::Census), bytes of host image memory by class.
inline std::atomic<uint64_t> census_gpu_tiled{0};    ///< GPU-written tiled (the GC keeps them)
inline std::atomic<uint64_t> census_gpu_linear{0};   ///< GPU-written linear
inline std::atomic<uint64_t> census_streamed{0};     ///< sampled, T# with LOD streaming counters
inline std::atomic<uint64_t> census_other{0};        ///< other sampled images
inline std::atomic<uint64_t> census_below_base{0};   ///< mips below the T# base_level
inline std::atomic<uint64_t> census_below_minlod{0}; ///< mips below max(base_level, min_lod)
inline std::atomic<uint64_t> census_live{0};         ///< images used in the last few ticks
inline std::atomic<uint64_t> census_count{0};
inline std::atomic<uint64_t> desync_repairs{0}; ///< stuck write faults repaired (page_manager)
inline std::atomic<uint64_t> desync_legit{0}; ///< ... repeats left alone (tracker wants protection)
inline std::atomic<uint64_t> stack_reg_spins{0}; ///< stack registry writer spins (page_manager)
inline std::atomic<uint64_t> fault_grace_marks{0};    ///< pages made hot (repeat-fault grace)
inline std::atomic<uint64_t> fault_grace_cap_hits{0}; ///< ... refused by the budget
inline std::atomic<uint64_t> fault_grace_hot_upload_bytes{0}; ///< hot pages uploaded, kept writable
/// SOTC_CMD_STATS: system calls behind the command processor's time (SOTC sys line): page
/// protection changes (calls, bytes; made writable), texture cache lock (acquired, contended),
/// device memory allocations and frees (VMA callbacks).
inline std::atomic<uint64_t> sys_protects{0}, sys_protect_bytes{0}, sys_unprotects{0};
inline std::atomic<uint64_t> sys_tc_locks{0}, sys_tc_contended{0}, sys_tc_wait_ns{0};
inline std::atomic<uint64_t> sys_vk_allocs{0}, sys_vk_frees{0};
/// ... protect calls from image page watchers (UpdatePageWatchers) and buffer regions.
inline std::atomic<uint64_t> sys_protect_src[2]{};
/// ... bytes invalidated: map hook, protect hook (the game's mprotect), HLE fills, all callers
/// of BufferCache::InvalidateMemory.
inline std::atomic<uint64_t> inv_map{0}, inv_protect{0}, inv_hle{0}, inv_buffer{0};
/// Mip-tail images (SOTC_MIP_TAIL, TextureCache::DecideRebase). Census gauges, bytes:
inline std::atomic<uint64_t> census_rebased{0};         ///< host memory of mip-tail images
inline std::atomic<uint64_t> census_rebased_count{0};
inline std::atomic<uint64_t> census_dropped{0};         ///< guest bytes of the mips they leave out
inline std::atomic<uint64_t> census_projected{0};       ///< what full-chain candidates would save
inline std::atomic<uint64_t> census_projected_hwcnt{0}; ///< ... of it, T#s with LOD counters
/// ... and counters (per stats line): tails created, refused (a full chain memoized, a GPU-written
/// overlap, a full chain in use, a layout RebaseToMip cannot reproduce), tails freed (a writer
/// overlapped them, an older tail of the same chain), tails re-uploaded instead of hashed
/// (MaybeCpuDirty), and broken never-written rules (cumulative).
inline std::atomic<uint64_t> rebase_created{0}, rebase_reuse_full{0}, rebase_skip_gpu{0},
    rebase_skip_live_full{0}, rebase_skip_layout{0}, rebase_writer_freed{0},
    rebase_dedup_freed{0}, rebase_maybe_dirty{0}, rebase_violations{0};

/// SOTC_OPT_OFF=1: the 2026-10-14 command-processor optimizations are switched off (replicate
/// HLE no-change skip, single texel-buffer image sync, rate-limited VRAM queries), for A/B runs
/// on one binary.
/// sotc: read once at static initialisation (as kCmdStats), so a check costs no guard: it ran per
/// buffer bind as a function-local static (~1.9% of the draw thread).
inline const bool kOptOff = [] {
    const char* v = std::getenv("SOTC_OPT_OFF");
    return v && v[0] == '1';
}();
inline bool OptOff() {
    return kOptOff;
}

struct ScopedTimer {
    std::atomic<uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit ScopedTimer(std::atomic<uint64_t>& t) : total{t} {}
    ~ScopedTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};

/// sotc: where the time of slow frames goes (SOTC_FPS, "SOTC FRAMES" line in videoout/driver.cpp).
/// Command-processor time per cause since the last flip; the flip reads and resets them.
/// shader: shader translation (CompileModule); pipeline: pipeline creation; upload: texture uploads (RefreshImage); gc:
/// emergency texture collections; readback: GPU-to-CPU downloads; image: new host images; buffer:
/// CPU-to-GPU buffer uploads of bound ranges; dma: DMA sweeps; alloc: arena and staging memory
/// allocation; hash: texture content hashes (RefreshImage); idle: waiting for the game to submit
/// work. A readback inside an upload counts in
/// both.
enum FrameCause {
    FrameCompile, FramePipeline, FrameUpload, FrameGc, FrameReadback, FrameImage, FrameBuffer,
    FrameDma, FrameAlloc, FrameHash, FrameIdle, FrameCauses
};
inline constexpr const char* kFrameCauseNames[FrameCauses] = {
    "shader", "pipeline", "upload", "gc",   "readback", "image",
    "buffer", "dma",      "alloc",  "hash", "idle"};
/// Bytes copied from guest memory to the GPU since the last flip (SynchronizeMemory).
inline std::atomic<uint64_t> frame_copy_bytes{0};
inline std::atomic<uint64_t> frame_ns[FrameCauses]{};
inline std::atomic<uint64_t> frame_compiles{0}; ///< shader modules and pipelines created
/// sotc: shader cache preload at boot (loading screen bar): pipelines to load and loaded.
inline std::atomic<uint32_t> warmup_total{0}, warmup_done{0};
/// sotc: SOTC_PRETRANSLATE boot hold (PipelineCache::MaybeHoldForPretranslate): the shader preparation screen
/// (bbport_overlay.cpp PrepScreen). Own counters, so the cache warm-up's bar never mixes with it. total grows while the
/// game still loads shader files.
inline std::atomic<bool> prep_active{false};
inline std::atomic<uint32_t> prep_total{0}, prep_done{0};
inline std::atomic<int64_t> prep_start_ms{0};
inline std::atomic<uint32_t> prep_writes{0}; ///< shader cache files still being written (the hold waits for them)
/// sotc: the boot's shader cache preload is over (the archive read-ahead waits for it: both
/// on one hard disk made the loading screen last 2 minutes).
inline std::atomic<bool> warmup_finished{false};
inline std::atomic<uint64_t> flip_count{0};     ///< game flips (videoout/driver.cpp Flip)
/// Game frames prepared by the command processor (vk_presenter.cpp PrepareFrame): unlike
/// flip_count, in step with the draws the command processor is processing.
inline std::atomic<uint64_t> cp_frame{0};
/// sotc: what the in-game performance overlay shows (bbport_overlay.cpp, show_fps / F10),
/// published by the SOTC_FPS printer at the end of each five-second window (videoout/driver.cpp).
inline std::atomic<float> ov_cmds_per_frame{0.0f};
inline std::atomic<float> ov_readbacks_per_s{0.0f};
inline std::atomic<float> ov_readback_ms_per_s{0.0f};
inline std::atomic<uint32_t> ov_compiles_window{0};
inline std::atomic<uint64_t> ov_compiles_total{0};
inline std::atomic<int> ov_worst_cause{-1}; ///< FrameCause that took most of the worst frame
inline std::atomic<float> ov_worst_cause_ms{0.0f};
/// DLSS Frame Generation (vk_dlssg.cpp): -1 not loaded, 0 off, 1 on, 2 error; frames shown per
/// game frame; sl::DLSSGStatus bits.
inline std::atomic<int> ov_fg_state{-1};
inline std::atomic<uint32_t> ov_fg_frames{0};
inline std::atomic<uint32_t> ov_fg_status{0};
/// FSR 3 frame generation (vk_fsrfg.cpp): 0 waiting for a 3D scene, 1 generating, 2 error.
inline std::atomic<int> ov_fsr_state{0};
/// Phases of PipelineCache::CompileModule (SOTC_FPS, "SOTC COMPILE" line): translate, emit SPIR-V,
/// shader patch lookup, vkCreateShaderModule, cache registration.
inline std::atomic<uint64_t> compile_phase_ns[5]{};
/// Memory allocations on the frame path per kind (SOTC_FPS, "SOTC ALLOC" line every 5 s):
/// count, bytes and time.
enum AllocKind { AllocArena, AllocStagingBlock, AllocStagingLarge, AllocKinds };
inline constexpr const char* kAllocKindNames[AllocKinds] = {"arena", "staging-block",
                                                            "staging-large"};
inline std::atomic<uint64_t> alloc_count[AllocKinds]{}, alloc_bytes[AllocKinds]{},
    alloc_ns[AllocKinds]{};
struct AllocTrace {
    AllocKind kind;
    bool timed = false; ///< sotc: the time is read only with kFpsTimers (SOTC_FPS_LEAN drops it)
    std::chrono::steady_clock::time_point start{};
    AllocTrace(AllocKind k, uint64_t bytes) : kind{k} {
        if (kFpsLog) {
            alloc_count[k].fetch_add(1, std::memory_order_relaxed);
            alloc_bytes[k].fetch_add(bytes, std::memory_order_relaxed);
            if (kFpsTimers) {
                timed = true;
                start = std::chrono::steady_clock::now();
            }
        }
    }
    ~AllocTrace() {
        if (timed) {
            alloc_ns[kind].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count(),
                                     std::memory_order_relaxed);
        }
    }
};

/// Adds the scope's duration to frame_ns[cause]; reads no clock unless kFpsTimers (SOTC_FPS, not
/// SOTC_FPS_LEAN). A timer
/// nested in one of the same cause on the same thread adds nothing (counted once).
struct FrameTimer {
    FrameCause cause;
    bool outer = false;
    std::chrono::steady_clock::time_point start{};
    static inline thread_local bool active[FrameCauses]{};
    explicit FrameTimer(FrameCause c) : cause{c} {
        if (kFpsTimers && !active[c]) {
            outer = active[c] = true;
            start = std::chrono::steady_clock::now();
        }
    }
    ~FrameTimer() {
        if (outer) {
            active[cause] = false;
            frame_ns[cause].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count(),
                                      std::memory_order_relaxed);
        }
    }
};

/// sotc: per-thread accounting of the GPU threads (SOTC_FPS only; "SOTC cpu/frame", "SOTC draw
/// waits/frame", "SOTC cp/frame", "SOTC recorder/frame" lines in videoout/driver.cpp, and the
/// per-frame figures of the SOTCSTALL lines in vk_presenter.cpp). Every counter below is
/// monotonic (never reset), so each printer keeps its own previous snapshot (FrameAccount).
///
/// Role of the calling thread, set at the start of each GPU thread (cheap thread_local; read
/// only behind kFpsLog). The CPU-time roles are also the BbThreadCpu::Register roles.
enum CpuRole : uint8_t {
    RoleOther, RoleCp, RoleDraw, RoleRecorder, RolePassSecondary, RoleUpload,
    RoleGuest, // sotc: SOTC_STALL_DETAIL, game guest threads (bbgpu_register_guest_thread);
               // their tl_role stays RoleOther
    CpuRoles
};
inline thread_local uint8_t tl_role = RoleOther;
/// Records the calling thread under `role` for the CPU-time sums (bbport_thread_cpu.cpp, which
/// is only told when SOTC_FPS is on) and sets tl_role.
void RegisterThreadRole(CpuRole role);

/// Waits of the draw thread (the thread that runs the back ops; the command processor itself
/// when the draw thread is off), by what it waits for:
/// gpu: GPU work to finish (semaphore and fence waits: Finish, FinishHostRead, readbacks,
///      SubmitAheadAndWait, command pool and descriptor reuse, eviction);
/// recorder: the recording thread (WaitSubmitted, Sync, chunk and fake handle backpressure);
/// upload: async upload copies and transfer-queue host copies;
/// tc: the texture cache lock held by another thread.
enum WaitKind : uint8_t { WaitGpu, WaitRecorder, WaitUpload, WaitTcLock, WaitKinds };
inline std::atomic<uint64_t> draw_wait_ns[WaitKinds]{}, draw_wait_count[WaitKinds]{};
/// Command processor time blocked on the draw thread (inside cp_parse_ns): BackAlloc while the
/// op ring is full, WaitBackTail/DrainBack. cp_parse_all_ns is cp_parse_ns, never reset.
inline std::atomic<uint64_t> cp_blocked_ns{0}, cp_parse_all_ns{0};
/// sotc: SOTC_FPS "SOTC CP ops" lines (Liverpool, SOTC_WD_FUSE). Func ops queued by kind (0 write
/// data, 1 EOP/EOS/release, 2 DMA, 3 other) and their run time on the draw thread; WRITE_DATA
/// packets fused into a draw; WRITE_DATA -> indirect draw/dispatch pairs; WaitBackWrites calls
/// that waited and their time.
inline std::atomic<uint64_t> cp_func_ops[4]{}, draw_func_ns[4]{};
inline std::atomic<uint64_t> cp_wd_fused{0}, cp_wd_pairs{0}, cp_bw_waits{0}, cp_bw_wait_ns{0};
/// sotc: "SOTC CP back-write waits" report line (driver.cpp), reset by each report: the
/// WaitBackWrites calls that waited (Liverpool::WaitBackWritesSlow), their wall time and the
/// longest one. Counted with SOTC_FPS; the time and the maximum need kFpsTimers (SOTC_FPS_LEAN
/// leaves them 0).
inline std::atomic<uint64_t> cp_bw_rep_waits{0}, cp_bw_rep_ns{0}, cp_bw_rep_max_ns{0};
/// sotc: command processor no-progress spin (Liverpool::Process; "SOTC CP spin" line, SOTCSTALL
/// "spin"). Written by the CP thread only (monotonic, store of load + n). spin: wall time of
/// streaks in which every resumed task only re-tested its wait (outside cp_parse_ns; sampled
/// every 32 resumes). rounds: whole rounds over the mapped queues without progress, and per
/// wait kind (Liverpool::CpWaitKind) the rounds in which a task waited on it. sleeps:
/// SOTC_CP_IDLE_US blocks, of which timeouts ended by the timeout; wakes/wake latency: blocks
/// ended by NotifyCpWait, and the time from its notification to the CP running again.
inline constexpr int CpSpinKinds = 5;
inline std::atomic<uint64_t> cp_spin_ns{0}, cp_spin_rounds{0}, cp_spin_kind_rounds[CpSpinKinds]{};
inline std::atomic<uint64_t> cp_sleeps{0}, cp_sleep_timeouts{0}, cp_sleep_ns{0}, cp_wakes{0},
    cp_wake_latency_ns{0};
inline void CpAdd(std::atomic<uint64_t>& counter, uint64_t n) {
    counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
/// Recording thread: time running blocks (not waiting for work), and inside it the waits for
/// async uploads before a submit (AsyncUpload::WaitUpTo) and vkQueueSubmit.
inline std::atomic<uint64_t> rec_busy_ns{0}, rec_upload_wait_ns{0}, rec_submit_ns{0};

/// Times a draw-thread wait into draw_wait_*[kind]. Exclusive: a wait nested in another (on the
/// same thread) is taken out of the outer one, so each nanosecond lands in one bucket. The wait
/// count needs only SOTC_FPS; the clock reads need kFpsTimers (or SOTC_TRACE) and the draw thread.
struct DrawWait {
    static inline thread_local uint64_t nested_ns = 0;
    uint64_t start = 0;
    uint64_t outer_nested = 0;
    WaitKind kind;
    bool on = false;    ///< counted: SOTC_FPS, on the draw thread
    bool timed = false; ///< clock read: kFpsTimers, or SOTC_TRACE (its span needs the times)
    explicit DrawWait(WaitKind k) : kind{k} {
        // sotc: SOTC_TRACE alone times the draw thread's waits for their spans (no counts).
        if ((kFpsLog || SotcTrace::kOn) && tl_role == RoleDraw) {
            on = kFpsLog;
            if (kFpsTimers || SotcTrace::kOn) { // sotc: SOTC_FPS_LEAN drops the clock reads
                timed = true;
                outer_nested = nested_ns;
                nested_ns = 0;
                start = NowNs();
            }
        }
    }
    ~DrawWait() {
        if (timed) {
            const uint64_t now = NowNs();
            const uint64_t elapsed = now - start;
            if (SotcTrace::kOn) { // sotc: SOTC_TRACE
                static constexpr const char* kNames[WaitKinds] = {"wait gpu", "wait recorder",
                                                                  "wait upload", "wait tc lock"};
                SotcTrace::Span(kNames[kind], start, now);
            }
            const uint64_t inner = nested_ns < elapsed ? nested_ns : elapsed;
            if (kFpsLog) { // sotc: the sums are read only by the SOTC_FPS printer
                draw_wait_ns[kind].fetch_add(elapsed - inner, std::memory_order_relaxed);
            }
            nested_ns = outer_nested + elapsed;
        }
        if (on) {
            draw_wait_count[kind].fetch_add(1, std::memory_order_relaxed);
        }
    }
    DrawWait(const DrawWait&) = delete;
    DrawWait& operator=(const DrawWait&) = delete;
};

/// Adds the scope's duration to `total` when SOTC_FPS is on and the caller has `role`. With
/// SOTC_TRACE only, the clock is read for the span (trace_name) and `total` is left alone.
/// sotc: trace_name (a string literal) also records the scope as a SOTC_TRACE span.
struct RoleTimer {
    std::atomic<uint64_t>& total;
    uint64_t start = 0;
    bool on = false;
    const char* trace_name = nullptr;
    RoleTimer(CpuRole role, std::atomic<uint64_t>& t, const char* trace = nullptr)
        : total{t}, trace_name{trace} {
        if ((kFpsTimers || SotcTrace::kOn) && tl_role == role) { // sotc: SOTC_FPS_LEAN
            on = true;
            start = NowNs();
        }
    }
    ~RoleTimer() {
        if (on) {
            const uint64_t now = NowNs();
            if (kFpsLog) { // sotc: the sums are read only by the SOTC_FPS printer
                total.fetch_add(now - start, std::memory_order_relaxed);
            }
            if (trace_name && SotcTrace::kOn) {
                SotcTrace::Span(trace_name, start, now);
            }
        }
    }
    RoleTimer(const RoleTimer&) = delete;
    RoleTimer& operator=(const RoleTimer&) = delete;
};

/// One reading of all the accounting counters; the difference of two is what happened between
/// them (Diff). Taken at every game flip by the SOTCSTALL code and per report by the printer.
struct FrameAccount {
    uint64_t cpu_ns[CpuRoles]{};
    uint64_t wait_ns[WaitKinds]{}, wait_count[WaitKinds]{};
    uint64_t parse_ns = 0, blocked_ns = 0;
    uint64_t spin_ns = 0; ///< sotc: command processor no-progress spin (cp_spin_ns)
    uint64_t rec_busy = 0, rec_upload = 0, rec_submit = 0;
    uint64_t fence = 0; ///< SOTC_ASYNC_UPLOAD write fence waits (async_upload_fence_ns)
    uint64_t rb_wait_ns = 0, rb_wait_count = 0; ///< sotc: guest readback waits (rb_guest_all_*)
    uint64_t rb_adj = 0; ///< sotc: adjacent guest readback requests (rb_pat_adjacent_all)
    uint64_t rb_pf = 0;  ///< sotc: SOTC_RB_PREFETCH prefetches consumed (rb_pf_consumed_all)
    uint64_t fault_count = 0, fault_ns = 0; ///< sotc: guest write faults (FaultStat::all_*)
    uint64_t flip_pace_ns = 0; ///< sotc: SOTC_STALL_DETAIL, cp_flip_pace_all_ns
    uint64_t upload_user_ns = 0, upload_kernel_ns = 0; ///< sotc: SOTC_STALL_DETAIL, RoleUpload threads
};
FrameAccount TakeAccount();
/// `now - before`, per field (saturating at zero).
FrameAccount DiffAccount(const FrameAccount& now, const FrameAccount& before);

/// SOTC_DROP_TRACE=1: logs the first 80 draws/dispatches of each kind that are dropped.
inline void Drop(int kind, unsigned long long a, unsigned long long b) {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_DROP_TRACE");
        return v && v[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::atomic<int> shown[8]{};
    static const char* names[8] = {"draw-nopipeline", "draw-nobind", "dispatch-nopipeline",
                                   "dispatch-nobind", "condexec-skip", "dispatch-hle", "?", "?"};
    if (shown[kind & 7].fetch_add(1) < 80) {
        std::fprintf(stderr, "SOTC drop %s %#llx %#llx\n", names[kind & 7], a, b);
    }
}
/// sotc: SOTC_RB_GROUP ("SOTC RB group" line, buffer_cache.cpp RbgAtFault), since the last report.
/// delivered: group downloads recorded (first_hit: at the first fault of a bind whose key was read
/// in pieces before; second_hit: at the second fault of a bind), with the ranges of their groups,
/// the page runs and bytes they copied (bytes_max: the largest). applied/applied_bytes: group
/// downloads written to guest memory. faults: guest read faults that landed on a known group; late:
/// ... on one already delivered (they raced the apply or hit a page left out). learned: keys found
/// hot. fb_*: groups not delivered, by reason (cold: read in one window so far; old: past the
/// freshness bound; size: too many ranges, or cut at the caps; cap: too many downloads in flight
/// or staging; empty: nothing left to copy; dirty: every page trimmed by later writes; expired:
/// never complete in time). skip_*: pages left out (readback window, owned by a readback in
/// flight, CPU bit, owed bytes, bytes not written by a fresh group). trimmed: bytes taken out by
/// later writes, unmaps or applies.
inline std::atomic<uint64_t> rbg_delivered{0}, rbg_first_hit{0}, rbg_second_hit{0},
    rbg_ranges{0}, rbg_runs{0}, rbg_bytes{0}, rbg_bytes_max{0}, rbg_applied{0},
    rbg_applied_bytes{0}, rbg_faults{0}, rbg_late{0}, rbg_learned{0}, rbg_trimmed{0};
inline std::atomic<uint64_t> rbg_fb_cold{0}, rbg_fb_old{0}, rbg_fb_size{0}, rbg_fb_cap{0},
    rbg_fb_empty{0}, rbg_fb_dirty{0}, rbg_expired{0};
inline std::atomic<uint64_t> rbg_skip_window{0}, rbg_skip_owned{0}, rbg_skip_cpu{0},
    rbg_skip_owed{0}, rbg_skip_stale{0};
/// sotc: SOTC_RB_GROUP: formats the "SOTC RB group" report line into out and resets the counters;
/// false (nothing written) when nothing happened (defined in buffer_cache.cpp).
bool RbGroupTake(char* out, std::size_t out_size);

/// sotc: SOTC_FPS "SOTC submit gate" line (gnmdriver.cpp GateWait): the game's time in the
/// submit gate, per site (0 command buffer submit, 1 compute doorbell, 2 SubmitDone), since the
/// last report. A call that took 5 us or more counts as a wait.
inline constexpr int GateSites = 3;
inline std::atomic<uint64_t> gate_calls[GateSites]{}, gate_waits[GateSites]{},
    gate_ns[GateSites]{}, gate_max_ns[GateSites]{};
/// SOTC_CP_AHEAD=2: frames closed by the guest but not yet recorded, sampled at SubmitDone.
inline std::atomic<uint64_t> gate_lag_sum{0}, gate_lag_samples{0}, gate_lag_max{0};
/// sotc: compute rings too full for a submit like the last one (Liverpool::NoteAscDoorbell):
/// windows that ended, their time and the longest; windows followed within 1 ms by a doorbell
/// on that ring (the game was most likely spinning in its retry loop, eboot 0x863a50) and the
/// time from the start of those windows to that doorbell; doorbells in all.
inline std::atomic<uint64_t> asc_full_windows{0}, asc_full_ns{0}, asc_full_max_ns{0},
    asc_full_spun{0}, asc_full_spun_ns{0}, asc_doorbells{0};
/// sotc: raises `max` to `value` (SOTC_FPS maxima above).
inline void GateStoreMax(std::atomic<uint64_t>& max, uint64_t value) {
    uint64_t seen = max.load(std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}
} // namespace SotcStats

// sotc: SOTC_HLE_SHADOW_CULL=1 (default off): the shadow-scene frustum cull compute shader is
// computed on the CPU at its place in the command stream (video_core/sotc_shadow_cull.cpp).
// Counters printed with SOTC_FPS (videoout/driver.cpp), bumped only while the switch is on.
namespace SotcStats {
inline const bool kShadowCullHle = [] {
    const char* v = std::getenv("SOTC_HLE_SHADOW_CULL");
    return v && v[0] == '1';
}();
/// Why a dispatch of that shader ran on the GPU instead.
enum ShadowCullFallback : int {
    ShadowCullLayout,   ///< resources, strides, dispatch shape or aliasing not the known ones
    ShadowCullInputGpu, ///< an input range is GPU-written (guest memory not authoritative)
    ShadowCullBounds,   ///< an access outside its buffer or an unmapped range
    ShadowCullReplace,  ///< the buffer cache refused the CPU write of an output
    ShadowCullRbPrev,   ///< SOTC_RB_PREV on: guest memory may be older than the GPU copy
    ShadowCullFallbacks
};
inline std::atomic<uint64_t> shadow_cull_dispatches{0}; ///< dispatches computed on the CPU
inline std::atomic<uint64_t> shadow_cull_sections{0};   ///< sections evaluated by them
inline std::atomic<uint64_t> shadow_cull_kept{0};       ///< set bits the GPU might have cleared
inline std::atomic<uint64_t> shadow_cull_ns{0};         ///< CPU time, fallbacks included
inline std::atomic<uint64_t> shadow_cull_fallbacks[ShadowCullFallbacks]{};
} // namespace SotcStats

// sotc: SOTC_CULL_EXPRESS=1 (default off): the XPR occlusion pass (a submit ending in a dispatch and
// a CS_DONE end-of-pipe label) is recorded and submitted AHEAD of the older ops still waiting in
// the back ring when that is provably unobservable (Liverpool::XpPoll, liverpool.cpp). Counters
// printed with SOTC_FPS (videoout/driver.cpp), bumped only while the switch is on.
namespace SotcStats {
inline const bool kCullExpress = [] {
    const char* v = std::getenv("SOTC_CULL_EXPRESS");
    return v && v[0] == '1';
}();
/// Why a published express segment ran in FIFO order (or at all).
enum XpReason : int {
    XpNoBacklog,      ///< the draw thread was already at (or inside) the segment
    XpSegTooLong,     ///< the segment has more ops than SOTC_CULL_EXPRESS_SEG
    XpSkipTooMany,    ///< more skipped ops than SOTC_CULL_EXPRESS_SKIP
    XpBudget,         ///< the decision took longer than SOTC_CULL_EXPRESS_US
    XpSkipUnknownFunc,///< a skipped Func op the lane cannot classify (flip, DMA to GDS, ...)
    XpSkipSignals,    ///< a skipped label/flip/mark and SOTC_CULL_EXPRESS_SIGNALS=0 / _MARKS=0
    XpSkipNotReady,   ///< a skipped draw/dispatch has no footprint yet (prep thread behind)
    XpSkipUnknownFp,  ///< a skipped draw/dispatch whose footprint is unknown
    XpExpressUnknown, ///< an express op that is not a draw, dispatch, label or known write
    XpExpressNotReady,///< an express draw/dispatch has no footprint yet
    XpExpressUnknownFp,///< an express draw/dispatch whose footprint is unknown
    XpConflict,       ///< a skipped op's footprint conflicts with the express ops'
    XpLabelSame,      ///< a skipped label writes the address of an express label (per-address FIFO)
    XpNoPrep,         ///< no GpuPrep threads (SOTC_PARALLEL_RECORD=1 needed)
    XpOk,             ///< ran ahead
    XpReasons
};
// sotc: SOTC_OP_OVERLAP=1 (plan step 7c.0, docs/DRAW-THREAD-7C-DESIGN.md, open question 5).
// Measurement only, default off; needs SOTC_PARALLEL_RECORD=1 (the prep threads fill the
// footprints) and SOTC_FPS=1 (the report). The draw thread compares each draw/dispatch's guest
// footprint with the previous op's (AmdGpu::Conflicts): the share of conflicting pairs bounds what
// a pipeline of draw stages could overlap. It adds work on the draw thread, so do not time
// anything in the same session.
inline const bool kOpOverlap = [] {
    const char* v = std::getenv("SOTC_OP_OVERLAP");
    return v && v[0] == '1';
}();
inline std::atomic<uint64_t> ovl_ops{0};       ///< draws/dispatches whose footprint was computed
inline std::atomic<uint64_t> ovl_not_ready{0}; ///< the op's prep slot was not ready (no footprint)
inline std::atomic<uint64_t> ovl_unknown{0};   ///< footprint unknown: would block any overlap
inline std::atomic<uint64_t> ovl_pairs{0};     ///< consecutive pairs with both footprints known
inline std::atomic<uint64_t> ovl_conflicts{0}; ///< ... where one op's writes overlap the other op
inline std::atomic<uint64_t> ovl_breaks{0};    ///< pairs cut by a command-processor op between them

// sotc: SOTC_AB_PR=1 (plan step 7c.0). In-session A/B of SOTC_PARALLEL_RECORD, default off; needs
// SOTC_PARALLEL_RECORD=1 and SOTC_FPS=1. The 5 s report alternates the phase: in even windows the
// draw thread uses the prepared draws, in odd windows it ignores them and computes inline (the
// path it already takes when a prepared draw is not ready). Each report starts with a
// "SOTC AB PR window N: on|off" line so the windows can be compared offline.
inline const bool kAbPr = [] {
    const char* v = std::getenv("SOTC_AB_PR");
    return v && v[0] == '1';
}();
inline std::atomic<uint32_t> ab_window{0};      ///< report window index; odd = prepared draws ignored
inline std::atomic<uint64_t> ab_ignored{0};     ///< prepared draws ignored in this window
inline bool AbPrIgnoring() noexcept {
    return kAbPr && (ab_window.load(std::memory_order_relaxed) & 1) != 0;
}

inline std::atomic<uint64_t> xp_segments{0};      ///< express segments published by the parser
inline std::atomic<uint64_t> xp_ahead{0};         ///< ... that ran ahead of older ops
inline std::atomic<uint64_t> xp_skipped_ops{0};   ///< older ops they jumped over
inline std::atomic<uint64_t> xp_express_ops{0};   ///< ops run ahead
inline std::atomic<uint64_t> xp_reasons[XpReasons]{};
/// Skipped ops that were labels / flips / frame marks (the progress signals of rule 3).
inline std::atomic<uint64_t> xp_sk_labels{0}, xp_sk_flips{0}, xp_sk_marks{0};
/// Queue-to-execution latency of the express EOP: ahead (sum, count) and in FIFO order.
inline std::atomic<uint64_t> xp_lat_ahead_ns{0}, xp_lat_fifo_ns{0}, xp_lat_fifo_n{0};
/// Estimated time saved: skipped ops times the draw thread's EWMA time per op.
inline std::atomic<uint64_t> xp_saved_est_ns{0};
/// Time of the decisions (footprints and conflict checks) and of the express runs.
inline std::atomic<uint64_t> xp_decide_ns{0}, xp_decide_max_ns{0}, xp_run_ns{0};
/// XpConflict split. By direction: an express write hits a skipped read (WAR, e.g. a frame N-1
/// dispatch still reading the HiZ the express pass overwrites), a skipped write hits an express
/// read (RAW), both write (WAW). By the kind of the first conflicting skipped op.
inline std::atomic<uint64_t> xp_conf_war{0}, xp_conf_raw{0}, xp_conf_waw{0};
inline std::atomic<uint64_t> xp_conf_draw{0}, xp_conf_dispatch{0}, xp_conf_func{0};
} // namespace SotcStats

// sotc: SOTC_RB_EXACT=publish counters (lean XPR publish mode). Written by BufferCache (label
// downloads, readbacks) and Liverpool (deferred CS_DONE labels, latency), printed with SOTC_FPS
// by the "SOTC RB publish" line of videoout/driver.cpp.
namespace SotcStats {
inline std::atomic<uint64_t> rbp_labels_deferred{0};   ///< deferred CS_DONE labels that landed
inline std::atomic<uint64_t> rbp_bytes_published{0};   ///< bytes applied by label downloads
inline std::atomic<uint64_t> rbp_dropped_dirty{0};     ///< label downloads dropped: a later write made them dirty
inline std::atomic<uint64_t> rbp_cpu_pages_skipped{0}; ///< pages skipped at apply because the CPU bit was set
inline std::atomic<uint64_t> rbp_lat_cp_ns{0};         ///< sum: CP parsed the EOP -> draw thread processed it
inline std::atomic<uint64_t> rbp_lat_submit_ns{0};     ///< sum: draw thread processed -> Flush() returned
inline std::atomic<uint64_t> rbp_lat_gpu_ns{0};        ///< sum: Flush() returned -> RbxDrain saw the tick done
inline std::atomic<uint64_t> rbp_lat_land_ns{0};       ///< sum: tick done -> label written
inline std::atomic<uint64_t> rbp_lat_samples{0};       ///< number of labels in the sums above
inline std::atomic<uint64_t> rbp_reads_before{0};      ///< guest readbacks on bytes of a label download not yet applied
inline std::atomic<uint64_t> rbp_reads_after{0};       ///< guest readbacks that still faulted on a page a label download applied earlier
} // namespace SotcStats

// sotc: SOTC_DRAW_WORK=1 (off by default): the "SOTC DRAW WORK" line of the 5 s report
// (videoout/driver.cpp, next to "SOTC threads per frame") and the op counts of the SOTCSTALL
// lines (vk_presenter.cpp). Needs SOTC_FPS=1 (draw_idle_ns) and SOTC_DRAW_SPLIT=1 (the operation
// and wait timers); without them the report says so. Counting only.
namespace SotcStats {
inline const bool kDrawWork = std::getenv("SOTC_DRAW_WORK") != nullptr;
/// Ops the draw thread executed, by SplitDraw, SplitIndirect, SplitDispatch, SplitFunc (index 0-3),
/// cumulative: DrawWorkReport takes the difference since its last call.
inline std::atomic<uint64_t> draw_work_ops[4]{};
/// The same ops since the last TakeFrameDrawOps (one frame, for SOTCSTALL).
inline std::atomic<uint64_t> frame_draw_ops[4]{};
/// Called by Liverpool::BackLoop for each op it executes (not for the ops that express execution
/// replays without running). `part` is the SplitPart the op's timer uses.
inline void NoteDrawOp(SplitPart part) {
    if (kDrawWork) {
        const int i = static_cast<int>(part) - static_cast<int>(SplitDraw);
        draw_work_ops[i].fetch_add(1, std::memory_order_relaxed);
        frame_draw_ops[i].fetch_add(1, std::memory_order_relaxed);
    }
}
/// Ops of one frame: draws (direct and indirect), dispatches and func ops. Zero without SOTC_DRAW_WORK.
struct FrameDrawOps {
    uint64_t draws, dispatches, funcs;
};
/// Takes and resets the frame's op counts. Call once per presented frame (vk_presenter.cpp, SOTCSTALL).
inline FrameDrawOps TakeFrameDrawOps() {
    FrameDrawOps out{};
    if (kDrawWork) {
        // Indices as in NoteDrawOp: 0 SplitDraw, 1 SplitIndirect, 2 SplitDispatch, 3 SplitFunc.
        out.draws = frame_draw_ops[0].exchange(0, std::memory_order_relaxed) +
                    frame_draw_ops[1].exchange(0, std::memory_order_relaxed);
        out.dispatches = frame_draw_ops[2].exchange(0, std::memory_order_relaxed);
        out.funcs = frame_draw_ops[3].exchange(0, std::memory_order_relaxed);
    }
    return out;
}
/// The "SOTC DRAW WORK" line for the window since the previous call, or "" (first call, flips 0,
/// switch off). Call it once per 5 s report with the frame count and the window's draw idle time
/// (the idle_ms of "SOTC threads per frame", which the caller resets). All values are per frame:
///   X = (window - idle - flip wait) / flips   draw thread work
///   Y = ops: ExecuteBackOp by kind, waits inside the ops included (SplitDraw..SplitFunc)
///   Z = commands: the ProcessCommands callbacks on the draw thread (SplitCommands)
///   R = guest readback waits, the four SplitRb parts (guest threads' waits, not in X; the
///       draw thread's readback work is inside Z)
///   F = flip wait on the draw thread (SplitWaitFrame; zero when the draw thread never blocks there)
///   N = draws, indirect draws and dispatches (func ops not counted)
/// X is more than Y + Z: the loop's own overhead and the ops replayed by express execution are
/// in X only. The window is the time between two calls, not the report's `seconds`.
inline std::string DrawWorkReport(uint64_t flips, double idle_ms) {
    if (!kDrawWork || flips == 0) {
        return {};
    }
    if (!kFpsTimers || !kDrawSplit) {
        return "SOTC DRAW WORK: unavailable (needs SOTC_FPS=1 and SOTC_DRAW_SPLIT=1)";
    }
    static uint64_t last_ns = 0, last_tick = 0;
    static uint64_t last_ticks[SplitParts]{};
    static uint64_t last_ops[4]{};
    const uint64_t now_ns = NowNs(), now_tick = Ticks();
    uint64_t now_ticks[SplitParts]{};
    for (int c = 0; c < SplitParts; ++c) {
        now_ticks[c] = split_ticks[c].load(std::memory_order_relaxed);
    }
    uint64_t now_ops[4]{};
    for (int i = 0; i < 4; ++i) {
        now_ops[i] = draw_work_ops[i].load(std::memory_order_relaxed);
    }
    std::string text;
    if (last_ns != 0 && now_ns > last_ns && now_tick > last_tick) {
        const double window_ms = static_cast<double>(now_ns - last_ns) / 1e6;
        const double ticks_per_ms = static_cast<double>(now_tick - last_tick) / window_ms;
        const auto ms = [&](SplitPart part) {
            return static_cast<double>(now_ticks[part] - last_ticks[part]) / ticks_per_ms;
        };
        const double frames = static_cast<double>(flips);
        const double flip_ms = ms(SplitWaitFrame);
        const double work = (window_ms - idle_ms - flip_ms) / frames;
        const double ops = (ms(SplitDraw) + ms(SplitIndirect) + ms(SplitDispatch) + ms(SplitFunc)) / frames;
        const double commands = ms(SplitCommands) / frames;
        const double readbacks =
            (ms(SplitRbQueue) + ms(SplitRbSubmit) + ms(SplitRbGpu) + ms(SplitRbFinish)) / frames;
        double op_count = 0;
        for (int i = 0; i < 3; ++i) { // draws, indirect draws, dispatches
            op_count += static_cast<double>(now_ops[i] - last_ops[i]);
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "SOTC DRAW WORK: %.2f ms/frame (ops %.2f ms, commands %.2f ms, readbacks %.2f ms, "
                      "flip wait %.2f ms; ops %.0f/frame)",
                      work, ops, commands, readbacks, flip_ms / frames, op_count / frames);
        text = buf;
    }
    last_ns = now_ns;
    last_tick = now_tick;
    std::copy(std::begin(now_ticks), std::end(now_ticks), std::begin(last_ticks));
    std::copy(std::begin(now_ops), std::end(now_ops), std::begin(last_ops));
    return text;
}
} // namespace SotcStats
