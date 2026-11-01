// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PC_RING full ring, the copy side. See pc_ring_copy.h.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "../../../sotc/mem_classes.h" // sotc: SOTC_PC_RING8_HEAPS (heap granule runs)
#include "../../../sotc/pc_ring.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/buffer_cache/pc_ring_copy.h"
#include "video_core/renderer_vulkan/sotc_guest_memory.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern "C" int runtime_memory_hold_range(uint64_t guest_addr, uint64_t size); // src/runtime_memory.c

namespace VideoCore::PcRingCopy {

namespace {

// Copies are cut at 1 MiB boundaries, so none crosses an arena page (the buffer cache asserts
// that a buffer request spans at most two arena pages, and arena pages are far larger).
constexpr u64 kChunkSize = u64{1} << 20;
constexpr u64 kPageSize = u64{1} << 12; // the tracking page (region manager BYTES_PER_PAGE)

// Copies [addr, addr + size) from the imported guest memory into the arena buffer holding it.
// False when the imported view cannot serve the span (the caller then falls back to the CPU path).
bool CopyChunk(Vulkan::Rasterizer& rasterizer, BufferCache& cache, u64 addr, u64 size) {
    vk::Buffer src{};
    u64 src_offset = 0;
    if (!Vulkan::SotcGuestMemory::LookupGuest(addr, size, &src, &src_offset)) {
        return false;
    }

    // ObtainBuffer finds the arena buffer and the offset of the span, making its blocks resident.
    // is_written is false: this is a host-data upload, not a GPU write (a GPU write would mark the
    // region GPU-modified and force a readback later). Its CPU sync may upload CPU-modified pages
    // of the span first; the copy below writes the same guest bytes over them.
    const auto [arena, arena_offset] = cache.ObtainBuffer(addr, static_cast<u32>(size), false,
                                                          false);
    if (!arena) {
        return false;
    }

    // The same barriers as Runtime::CopyBuffer (vk_runtime.cpp): the render pass ends, pending
    // barriers flush when the arena range was used since its last barrier, and the write is
    // recorded for the next users. The imported source needs no tracking: the game wrote it on
    // the host before the submit, and queue submission makes host writes visible to the GPU.
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = rasterizer.GetScheduler();
    scheduler.EndRendering();
    if (runtime.IsBufferAccessed(arena, arena_offset, size, true)) {
        runtime.FlushBarriers();
    }
    const vk::BufferCopy copy{src_offset, arena_offset, size};
    scheduler.CommandBuffer().copyBuffer(src, arena->Handle(), std::span{&copy, 1});
    runtime.AccessBuffer(arena, arena_offset, size, vk::PipelineStageFlagBits2::eCopy,
                         vk::AccessFlagBits2::eTransferWrite);
    return true;
}

// One extent of the submit: copied chunk by chunk. A chunk that cannot be copied is marked
// CPU-modified, so the existing upload path copies it at its bind.
void CopyExtent(Vulkan::Rasterizer& rasterizer, BufferCache& cache, u64 addr, u64 size) {
    const u64 end = addr + size;
    for (u64 pos = addr; pos < end;) {
        const u64 next = std::min(end, (pos & ~(kChunkSize - 1)) + kChunkSize);
        const u64 len = next - pos;
        if (CopyChunk(rasterizer, cache, pos, len)) {
            cache.PcRingMarkClean(pos, len);
            SotcPcRing::NoteRingCopy(len, true);
        } else {
            cache.InvalidateMemory(pos, len); // fallback: CPU-modified, uploaded at the bind
            SotcPcRing::NoteRingCopy(len, false);
        }
        pos = next;
    }
}

// --- Modes 6 and 7: protect then copy (K = 1) ----------------------------------------------------
//
// Terms. For each 4 KiB page p of the arena: c(p) = the CPU bit (dirty: the next bind of a range
// holding p uploads it), g(p) = the GPU bit, r(p) = the ring bit (RegionManager::SetRingMask). The
// page is write-protected by the host iff P = !c && (!r || g). So a ring page with no GPU data is
// unprotected: a guest write to it does not fault and sets no CPU bit.
//
// Takes. Take k is the TakeSubmitExtents call at the start of the processing of submit k. Let e_k
// be the epoch OnSubmit set for submit k (before the submit reaches the draw thread). A take returns
// every extent recorded with an epoch <= the current one, so take k returns every extent recorded
// before submit k's call (their epoch is <= e_k - 1 < e_k), and possibly some recorded later.
//
// Entry. A whole page covered by one allocation is queued by the heap hook, on the allocating thread,
// before its extent is recorded (RecordResult, EnterPush in pc_ring.cpp). The game thread does no
// ring work. The draw thread pops the queue in each take and enters the pages (step (1) below). A
// page the allocation covers in part is not queued: it keeps P = !c, so its writes fault as before.
// An entry the full queue refuses is dropped: its pages keep P = !c too.
//
// Protocol at take k (OnSubmitStart, in this order; all on the draw thread):
//   (0) TakeSubmitExtents gives E_k, then TakeRingEnters pops the queued entries N_k. The order is
//       the proof's: an extent of E_k was recorded after its entry was queued, so its entry is in N_k
//       or was popped by an earlier take (lemma L1).
//   (1) enter: the ring bit of the pages of N_k is set (AddRingRange). A page with c = 0 and g = 0
//       is unprotected from here (P = 0); a page with c = 1 keeps P = 0 as it is.
//   (2) MarkRingClean on the pages of E_k, then leave the ring (RemoveRingRange): the ring bit of
//       each page of E_k is cleared and the page is protected again (P = 1 unless c = 1). Clean bits
//       are cleared while the page is still ring, so the page stays unprotected during the clear
//       and nothing is protected twice.
//   (3) snapshot of E_k (verify, mode 7 only), taken after (2): it holds the bytes the copy sees.
//   (4) the copies of E_k from imported guest memory to VRAM (CopyExtent). The GPU reads the guest
//       memory when it executes the copy, which is after this command buffer is submitted.
//
// Lemma L1. For an entry e of the pages of an extent X: e is popped by a take i <= j, where j is the
//   take that returns X. e is pushed before X is recorded, X is recorded under g_ext_locked before
//   take j takes that lock, and take j pops only after it has taken the lock. So the push happens
//   before take j's pop, and every pop of the queue after it (take j, or an earlier one whose pop
//   ran after the push) sees e. An entry popped by take i is applied at (1) of take i, before any
//   later step of take i.
//
// Invariants.
//   I1. A page's ring bit is set only by an entry's (1), and every such set is followed by a clear:
//       the clear of the take j that returns the entry's extent (2), or an earlier clear by another
//       take's (2) for the same page. Clearing is a no-op on a page that is not ringed. So no page
//       stays ringed without a consumption, and a ringed page is consumed by a later take's copy.
//   I2. After (2) of take k, the pages of E_k are protected (r = 0, and P = 1 or c = 1 on a page
//       that a fault marked dirty since).
//   I3. A write to a page after (2) of take k either faults (P = 1: sets c = 1, the page is then
//       uploaded by the next bind that covers it) or lands on a page already dirty (c = 1: uploaded
//       by the next such bind). Either way the next bind of a range holding the page uploads the
//       page, so the write is seen.
//   I4. A ring interval of a page ends with a copy that sees every write made in it. The bit is
//       set at (1) and cleared at (2) of a take j; the copy of j (4) executes after its submit, which
//       is after (2). A write made while the bit is set is made before (2), so it is in host memory
//       when the copy executes. A write after (2) faults (I2, I3) or lands on a dirty page.
//       The ring therefore never hides a write from the copy that consumes the page.
//
// Proof that submit k reads correct bytes. Take a guest byte b that submit k's shaders read, on
// page p. The game wrote b before submit k's call (the command buffer and its descriptors refer
// to the bytes the game produced before the call; assumption A0: the game does not reuse memory
// that a submitted job still reads before that job completes, which its own fences guarantee).
// Two cases.
//   (a) p was allocated before submit k's call. Its extent was recorded before submit k's call, with
//       an epoch below e_k, so take k returns it or an earlier take did (E_k or an earlier E_j: the
//       take of its entry and the take returning it are as in L1). So p is consumed by a take j <= k,
//       and p is protected after that consumption (I2). Let t be the time of the last consumption of
//       p before the start of take k (t is the consumption (2) of the take j, and the copy of (4) of
//       j comes after it).
//       - Every write to p before t is in host memory before the copy of (4) of take j executes: the
//         copy reads all bytes of the extent, the ringed writes are before (2) of j (I4), and the
//         writes before the page was ringed were faults that the copy also covers (it copies the
//         whole extent, not only the dirty pages). The GPU reads host memory when it executes the
//         copy, so the VRAM copy holds those writes.
//       - Every write to p after t faults (I3) or lands on a dirty page (I3). The next bind of a
//         range holding p uploads p, so the GPU reads the current bytes at that bind or later.
//       - A bind of k that covers p before any such write reads the copy, which is current for
//         every write before t (and such writes are not possible for p while it is protected).
//       So b is correct at its bind. (Writes concurrent with the GPU's own use of b are unordered,
//       as before: the normal path has the same window.)
//   (b) p was allocated after submit k's call. Then the allocation was recorded after the call
//       (the heap hook records it on the allocating thread before the game can write the memory and
//       submit it; the entry's push comes before that record, and the entry changes no byte). The game wrote b after that allocation, so submit k cannot read b (its bytes
//       were written before the call, A0). Such bytes need no copy. The bind of k may still cover
//       p (an arena bind range is larger than what the shader reads, and the bump allocator writes
//       the next job inside it): a ring page is not uploaded there, so the VRAM bytes of p are
//       unrelated to the shader's reads. This is assumption A1: the shaders read only bytes the
//       game wrote before the submit. Mode 7 cannot check it (no snapshot covers these bytes).
//
// Races checked. (i) The game thread and the draw thread share only the entry queue (lock-free) and
//   the extent spinlock; all ring and protection changes are on the draw thread. A guest write that
//   races with an entry either precedes it (a fault, handled as before, sets c = 1, and the entry
//   then leaves P = 0 on that dirty page) or follows it (no fault: I4, its bytes are copied by the
//   take that consumes the page). (ii) Two takes never race (one draw thread). (iii) The entry and
//   the consumption of other pages of one region are serialized by the region mutex, as in
//   SetRingRange. (iv) A queued entry whose extent is not yet recorded is popped early: the page is
//   ringed with no pending extent until its take (L1 gives that take), and no write is lost (I4).
//
// Why the entry is on the draw thread, not at the allocation. An entry changes page protection, which
// takes the region mutex and the runtime's table lock (runtime_memory_gpu_protect). Done on the game
// thread it made a cycle possible with the guest faults that the draw thread handles: the game thread
// holds the region mutex and waits for the table lock; a guest thread holding the table lock waits
// for the draw thread (a fault is handled there); the draw thread waits for the region mutex in this
// take. The stall of 2026-11-01 (user/pcmem/20261101-042755/logs/ring7-r1-a1.log, no flip after the
// arena block set changed) fits that cycle; it was not reproduced. With the entry on the draw
// thread the game thread takes neither lock on the ring's behalf.
// Cost: the writes between an allocation and the next take fault again, as without the ring. The
// ring covers a page from the first take after its allocation (before, from the allocation). The
// saving of the allocation window is smaller; measure it with SOTC FPS.
//
// Pages of E_k that were never entered (partly covered pages, whole-block fallbacks, dropped
// entries) are protected normally by (2); (2) on them is a no-op for the ring, and their copy is
// still correct: (3) and (4) hold for any page that is protected at (2).

// The BufferCache is set on the draw thread before the first take's entries are applied
// (ProtectThenCopy).
std::atomic<BufferCache*> g_cache{nullptr};

// Mode 7 verify probe (draw thread): true when a CPU bit is set in the page. The bind uploads it.
bool CpuDirty(uint64_t addr, uint64_t size) {
    BufferCache* cache = g_cache.load(std::memory_order_acquire);
    return cache && cache->IsRegionCpuModified(addr, size);
}

// The guest memory of the arena blocks is imported when the set of blocks changes (as mode 3).
void ImportArenaBlocks() {
    static u64 seen_version = 0;
    u64 version = seen_version;
    const auto blocks = SotcPcRing::ArenaBlocks(version);
    if (version == seen_version) {
        return;
    }
    seen_version = version;
    static std::vector<u64> held;
    for (const auto& block : blocks) {
        // sotc: SOTC_COMMIT_CHUNKS: the imported pages stay committed while the GPU may read them.
        if (std::find(held.begin(), held.end(), block.addr) == held.end() &&
            runtime_memory_hold_range(block.addr, block.size) == 0) {
            held.push_back(block.addr);
        }
        Vulkan::SotcGuestMemory::ImportGuestRange(block.addr, block.size);
    }
}

// The page-aligned spans of the extents, sorted and merged (disjoint).
std::vector<SotcPcRing::RingExtent> PageSpans(const std::vector<SotcPcRing::RingExtent>& extents) {
    std::vector<SotcPcRing::RingExtent> spans;
    spans.reserve(extents.size());
    for (const auto& extent : extents) {
        const u64 first = extent.addr & ~(kPageSize - 1);
        const u64 end = (extent.addr + extent.size + kPageSize - 1) & ~(kPageSize - 1);
        spans.push_back(SotcPcRing::RingExtent{first, end - first});
    }
    std::sort(spans.begin(), spans.end(),
              [](const SotcPcRing::RingExtent& a, const SotcPcRing::RingExtent& b) {
                  return a.addr < b.addr;
              });
    std::vector<SotcPcRing::RingExtent> merged;
    for (const auto& span : spans) {
        if (!merged.empty() && span.addr <= merged.back().addr + merged.back().size) {
            const u64 end = std::max(merged.back().addr + merged.back().size, span.addr + span.size);
            merged.back().size = end - merged.back().addr;
        } else {
            merged.push_back(span);
        }
    }
    return merged;
}

// Modes 6 and 7, at the start of a submit (draw thread): steps (0) to (4) of the proof above. Called
// right after TakeSubmitExtents, which gave `extents` (E_k); the entries are popped here, after it.
void ProtectThenCopy(Vulkan::Rasterizer& rasterizer, BufferCache& cache, int mode,
                     const std::vector<SotcPcRing::RingExtent>& extents) {
    // Installed on the first submit. The ring switch is on before any page is entered.
    if (!g_cache.load(std::memory_order_acquire)) {
        RegionManager::ring_enabled.store(true, std::memory_order_release);
        g_cache.store(&cache, std::memory_order_release);
        if (mode == 7) {
            SotcPcRing::SetRingDirtyProbe(&CpuDirty);
        }
    }
    ImportArenaBlocks();

    // (0) the queued entries, popped after this take's extents (lemma L1).
    std::vector<SotcPcRing::RingExtent> enters;
    SotcPcRing::TakeRingEnters(enters);
    // (1) enter: the ring bit of the pages of the entries. Spans are merged first (one tracker call
    // per span).
    for (const auto& span : PageSpans(enters)) {
        cache.PcRingDelta(span.addr, span.size, true);
    }

    // (2) Protect, then (3) snapshot (verify), then (4) copy. The consumption of the pages in the
    // spans is what the ring leaves: MarkRingClean first (it clears the CPU bits of ring pages with
    // no GPU data, while they are still unprotected), then RemoveRingRange (protect).
    const auto spans = PageSpans(extents);
    u64 pages = 0;
    for (const auto& span : spans) {
        cache.PcRingMarkClean(span.addr, span.size);
        cache.PcRingDelta(span.addr, span.size, false);
        pages += span.size / kPageSize;
    }
    SotcPcRing::NoteRingLeave(pages);

    SotcPcRing::SnapshotExtents(extents); // mode 7 only (no-op in mode 6)

    for (const auto& extent : extents) {
        CopyExtent(rasterizer, cache, extent.addr, extent.size);
    }
}

} // namespace

// sotc: SOTC_PC_RING8_HEAPS=<memtype heap names, comma separated, e.g. CPUMemory> (off by default; needs
// SOTC_HOOK_MEMTYPE_PROBE=1, SOTC_MEMTYPE_CLASSES=1 and SOTC_CLASS_TRACK=2 for the granules and their
// binds). The granules of those heaps that the GPU binds and never writes are served like arena blocks:
// imported, ringed (no protection) and read by the GPU in host memory. Granule faults measured 1 Nov: the
// write faults sit on CPU-written, GPU-read memory (memtype:CPUMemory about 2400/s), not on CPU-only memory.
// A run with GPU-modified pages in the tracker is left out: its newest bytes are in the device copy.
// Recollected at most every kHeapEvery submits, and only when the class state moved.
static void CollectHeapRuns(BufferCache& cache, uint32_t epoch) {
    static const std::vector<std::string> heaps = [] {
        std::vector<std::string> out;
        const char* v = std::getenv("SOTC_PC_RING8_HEAPS");
        std::string s = v ? v : "";
        size_t at = 0;
        while (at < s.size()) {
            const size_t comma = s.find(',', at);
            const std::string name = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!name.empty()) {
                out.push_back("memtype:" + name);
            }
            at = comma == std::string::npos ? s.size() : comma + 1;
        }
        return out;
    }();
    constexpr uint32_t kHeapEvery = 32;
    constexpr size_t kMaxRuns = 1024; // imports per heap set (each run is one vkAllocateMemory)
    static uint32_t last_epoch = 0;
    static uint64_t last_version = ~uint64_t{0};
    if (heaps.empty() || epoch - last_epoch < kHeapEvery ||
        SotcMemClasses::Version() == last_version) {
        return;
    }
    last_epoch = epoch;
    std::vector<SotcPcRing::RingExtent> runs;
    uint64_t version = 0;
    uint64_t gpu_modified = 0;
    for (const std::string& heap : heaps) {
        for (const auto& [addr, size] : SotcMemClasses::CollectHostServe(heap.c_str(), version)) {
            if (cache.IsRegionGpuModified(addr, size)) {
                ++gpu_modified;
                continue;
            }
            if (runs.size() < kMaxRuns) {
                runs.push_back({addr, size});
            }
        }
    }
    last_version = version;
    static int logged = 0;
    if (logged++ < 32) {
        u64 bytes = 0;
        for (const auto& r : runs) {
            bytes += r.size;
        }
        std::fprintf(stderr,
                     "SOTC PC RING8 heaps: %zu runs (%llu MiB) served, %llu runs left out (GPU-modified)\n",
                     runs.size(), static_cast<unsigned long long>(bytes >> 20),
                     static_cast<unsigned long long>(gpu_modified));
    }
    SotcPcRing::SetHostExtras(std::move(runs));
}

void HostMaintain(BufferCache& cache) {
    if (SotcPcRing::Mode() != 8) {
        return;
    }
    CollectHeapRuns(cache, SotcPcRing::SubmitEpoch());
    // Once per submit epoch, and whenever the arena block set changed. The epoch is read first: a
    // block that appears after it is seen at the next submit.
    static uint32_t seen_epoch = 0;
    static uint64_t seen_version = 0;
    static std::vector<SotcPcRing::RingExtent> applied; // the blocks the ring was applied to
    const uint32_t epoch = SotcPcRing::SubmitEpoch();
    if (epoch == seen_epoch && SotcPcRing::BlockVersion() == seen_version) {
        return;
    }
    seen_epoch = epoch;
    uint64_t version = 0;
    const auto blocks = SotcPcRing::ArenaBlocks(version);
    seen_version = version;

    // A block that is not imported (refused, or not yet possible) keeps normal tracking: it is not
    // ringed, and its binds take the normal path.
    static std::vector<u64> held;
    for (const auto& block : blocks) {
        // sotc: SOTC_COMMIT_CHUNKS: the imported pages stay committed while the GPU may read them.
        if (std::find(held.begin(), held.end(), block.addr) == held.end() &&
            runtime_memory_hold_range(block.addr, block.size) == 0) {
            held.push_back(block.addr);
        }
    }
    // Blocks that are adjacent in guest address space are imported as one range (one import call,
    // one run when the backing is contiguous), so a bind across their edge is served. A refused
    // range is retried block by block; the blocks that import are the ones ringed and served.
    auto sorted = blocks;
    std::sort(sorted.begin(), sorted.end(),
              [](const SotcPcRing::RingExtent& a, const SotcPcRing::RingExtent& b) {
                  return a.addr < b.addr;
              });
    std::vector<SotcPcRing::RingExtent> imported;
    u64 bytes = 0;
    for (size_t i = 0; i < sorted.size();) {
        size_t j = i;
        SotcPcRing::RingExtent group = sorted[i];
        while (j + 1 < sorted.size() && group.addr + group.size == sorted[j + 1].addr) {
            ++j;
            group.size += sorted[j].size;
        }
        // sotc: group imports are off unless SOTC_PC_RING8_GROUP=1 (SotcPcRing::Ring8Group): with
        // them on, ~1200 binds per 5 s straddled and took the full upload path (FPS 27 -> 10, 1 Nov).
        if (SotcPcRing::Ring8Group() && j > i &&
            Vulkan::SotcGuestMemory::ImportGuestRange(group.addr, group.size)) {
            imported.push_back(group);
            bytes += group.size;
        } else {
            for (size_t k = i; k <= j; ++k) {
                if (Vulkan::SotcGuestMemory::ImportGuestRange(sorted[k].addr, sorted[k].size)) {
                    imported.push_back(sorted[k]);
                    bytes += sorted[k].size;
                } else {
                    std::fprintf(stderr, "SOTC PC RING8 import refused: block %#llx +%llu KiB\n",
                                 static_cast<unsigned long long>(sorted[k].addr),
                                 static_cast<unsigned long long>(sorted[k].size >> 10));
                }
            }
        }
        i = j + 1;
    }
    SotcPcRing::NoteHostBlocks(imported.size(), bytes);

    bool same = imported.size() == applied.size();
    for (size_t i = 0; same && i < imported.size(); ++i) {
        same = imported[i].addr == applied[i].addr && imported[i].size == applied[i].size;
    }
    if (!same) {
        // One-shot per change (the first 64 changes per run): the imported set and its sizes.
        static int changes = 0;
        if (changes++ < 64) {
            std::fprintf(stderr, "SOTC PC RING8 host maintain: %zu blocks, %zu imported (%llu MiB), "
                                 "was %zu\n",
                         sorted.size(), imported.size(),
                         static_cast<unsigned long long>(bytes >> 20), applied.size());
            for (const auto& block : imported) {
                std::fprintf(stderr, "  imported %#llx +%llu KiB\n",
                             static_cast<unsigned long long>(block.addr),
                             static_cast<unsigned long long>(block.size >> 10));
            }
        }
        applied = imported;
        RegionManager::ring_enabled.store(true, std::memory_order_release);
        cache.PcHostApply(imported);
    }
}

void OnSubmitStart(Vulkan::Rasterizer& rasterizer) {
    // sotc: mode 8 (host binds): no extents and no copies. The submit start imports and rings the
    // blocks (HostMaintain, before any bind of the submit) and closes the previous submit's
    // statistics (unique bytes, verify samples).
    if (SotcPcRing::Mode() == 8) {
        auto& cache = rasterizer.GetBufferCache();
        cache.PcHostSubmitStart();
        // sotc: HostMaintain (import + hold of the arena blocks) is NOT run here: at submit start it hung
        // the boot (1 Nov, same signature as modes 6/7, which also import/hold here). It runs lazily
        // from the first bind of the submit (ObtainBuffer -> PcHostApply), which worked in game.
        return;
    }
    const int mode = SotcPcRing::FullRingMode();
    if (mode == 0) {
        return;
    }
    // sotc: the stall guard (RegionManager::LockTimed) reports waits of this thread in modes 6, 7.
    RegionManager::ring_take_thread = mode >= 6;
    auto& cache = rasterizer.GetBufferCache();

    std::vector<SotcPcRing::RingExtent> extents;
    SotcPcRing::TakeSubmitExtents(extents);

    if (mode == 6 || mode == 7) {
        ProtectThenCopy(rasterizer, cache, mode, extents);
        return;
    }

    if (mode == 4) {
        // Verify: the extents are only recorded; the protection and the copy are as before.
        SotcPcRing::SnapshotExtents(extents);
        return;
    }
    if (mode == 5) {
        // Hot-page ring with verify: only whole pages that one of the last K takes covers are ringed
        // (the hot mask); every other arena page keeps normal tracking. The snapshot feeds the bind
        // check. The mask changes by a delta per take (HotRingDelta): the pages that leave are
        // protected, the pages that join are unprotected, both before this submit's bytes are
        // copied, so a page leaving the ring has its later writes fault. A full re-apply replaces
        // the mask every 1024 submits and when the arena blocks change; it yields the same mask.
        SotcPcRing::SnapshotExtents(extents);
        RegionManager::ring_enabled.store(true, std::memory_order_release);
        static u64 seen_blocks = 0;
        u64 blocks_version = seen_blocks;
        const auto blocks = SotcPcRing::ArenaBlocks(blocks_version);
        const bool blocks_changed = blocks_version != seen_blocks;
        if (blocks_changed) {
            seen_blocks = blocks_version;
            static std::vector<u64> held;
            for (const auto& block : blocks) {
                if (std::find(held.begin(), held.end(), block.addr) == held.end() &&
                    runtime_memory_hold_range(block.addr, block.size) == 0) {
                    held.push_back(block.addr);
                }
                Vulkan::SotcGuestMemory::ImportGuestRange(block.addr, block.size);
            }
        }
        // Advances the hot window by this take (its extents are already in the history).
        std::vector<SotcPcRing::RingExtent> enter;
        std::vector<SotcPcRing::RingExtent> leave;
        SotcPcRing::HotRingDelta(enter, leave);
        static u32 submits_since_full = 0;
        if (blocks_changed || ++submits_since_full >= 1024) {
            submits_since_full = 0;
            std::vector<SotcPcRing::RingExtent> hot;
            u64 hot_version = 0;
            SotcPcRing::HotRingPages(hot, hot_version);
            cache.PcRingApply(hot);
        } else if (!leave.empty() || !enter.empty()) {
            const auto start = std::chrono::steady_clock::now();
            // Leaving first: a clean page is protected before any of this submit's copies.
            for (const auto& extent : leave) {
                cache.PcRingDelta(extent.addr, extent.size, false);
            }
            for (const auto& extent : enter) {
                cache.PcRingDelta(extent.addr, extent.size, true);
            }
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            SotcPcRing::NoteHotDeltaApply(static_cast<uint64_t>(ns));
        }
        for (const auto& extent : extents) {
            CopyExtent(rasterizer, cache, extent.addr, extent.size);
        }
        return;
    }

    // Mode 3. The ring protection rule is switched on before the first mask is applied: while every
    // ring bit is 0 the rule reduces to the old one, so turning it on here is safe.
    RegionManager::ring_enabled.store(true, std::memory_order_release);
    // The arena blocks are the ring's pages: when their set changed, the guest memory of each block
    // is imported and the tracker applies the ring to them. Regions created since the last apply
    // start without ring bits (normal protection: correct, just not ringed), so the mask is also
    // re-applied every 32 submits.
    static u64 seen_version = 0;
    static u32 submits_since_apply = 0;
    u64 version = seen_version;
    const auto blocks = SotcPcRing::ArenaBlocks(version);
    if (version != seen_version || ++submits_since_apply >= 32) {
        if (version != seen_version) {
            for (const auto& block : blocks) {
                // sotc: SOTC_COMMIT_CHUNKS: the imported pages stay committed while the GPU may
                // read them (the arena lives for the session; held once per block address).
                static std::vector<u64> held;
                if (std::find(held.begin(), held.end(), block.addr) == held.end() &&
                    runtime_memory_hold_range(block.addr, block.size) == 0) {
                    held.push_back(block.addr);
                }
                Vulkan::SotcGuestMemory::ImportGuestRange(block.addr, block.size);
            }
        }
        seen_version = version;
        submits_since_apply = 0;
        cache.PcRingApply(blocks);
    }

    for (const auto& extent : extents) {
        CopyExtent(rasterizer, cache, extent.addr, extent.size);
    }
}

} // namespace VideoCore::PcRingCopy
