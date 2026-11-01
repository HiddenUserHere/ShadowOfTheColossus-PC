// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC memory model, per-frame upload ring (docs/re-stutters/PC-MEMORY-REWRITE.md section 7,
// step 1). The per-frame GPU heaps (RenderSingleFrameGPU, GameToGPU, ParticleDoubleFrame) live in
// the kind-2 "Default TaggedArena" blocks. Today every CPU write to them is found by a write
// fault (48% of all write faults). The ring replaces that: the arena's pages are never
// write-protected, and at each submit they are declared CPU-written, so the binds of that submit
// upload them once.
//
// SOTC_PC_RING:
//   0  off (default).
//   2  shadow: nothing changes. The first upload of an arena page in a submit copies it to a
//      shadow of the arena; a later upload of it in the same submit compares the bytes inside the
//      bound range with that copy. A difference is "divergent": the ring would have left the GPU
//      with the older bytes of a range it reads. Must stay 0 over a session before mode 1.
//   1  apply: when the game allocates in the arena, the 4 KiB pages of the new allocation not
//      declared in this submit are unprotected and marked CPU-written before the game writes them
//      (PageManager::DeclareRingWrite, the fault's write branch ahead of time, settled on the
//      calling thread; SOTC_PC_RING_SLOW=1 uses PageManager::DeclareCpuWrite, which posts it to the
//      draw thread). Two sites, one
//      hook each (scripts/hooks.d/pcring.py). PC_RING_HEAP (default, SOTC_PC_RING=1): a wrap of the
//      RenderSingleFrameGPU heap's allocator slot, sub_1d750; every allocation of that heap passes
//      there, whatever the caller. PC_RING_BUMP (SOTC_PC_RING_BUMP=1, comparison only): an entry
//      of the frame bump allocator 0x48440, which the arena does not use. PC_RING_ALLOC
//      (SOTC_PC_RING_DISPATCH=1, comparison only): a wrap of 0x12820 for every allocation. Heap and
//      dispatch can run together (the range is declared twice, idempotent per submit). Protection
//      stays on: a page bound and rewritten within the submit still faults, so the GPU never sees
//      stale bytes (the shadow found 2-3 such pages per frame). Not together with
//      SOTC_HOOK_MEMTYPE_PROBE (same wrap site as PC_RING_ALLOC).
//   3  full ring: no CPU write declare and no protection change by the ring. The PC_RING_HEAP hook
//      (same hook as mode 1, same switch) records every allocation that lands in an arena block as
//      an extent of the current submit epoch; adjacent or overlapping extents of one epoch are
//      merged. TakeSubmitExtents hands the extents to the draw thread at the start of each submit.
//      Bounded: when the pending list is full, the allocation is not recorded, its whole arena
//      block is marked and emitted as one extent at the next take, and the overflow is counted.
//   4  full ring, verify: mode 3, plus SnapshotExtents (at each take, a copy of the taken extents'
//      bytes, kept for the last 4 takes) and CheckBind (a bind compares the guest bytes now with the
//      newest snapshot covering each byte of the bound range and counts the divergent bytes).
//      Only one bind in SOTC_PC_RING_VERIFY_SAMPLE (default 16) is checked. Also classifies the
//      sampled arena bytes by the age of the take whose extents cover them (coverage, see Report).
//   5  full ring, hot mask (safe-mask candidate): as mode 4, but the copy side protects the arena
//      pages that are not hot. A page is hot when one take of the last SOTC_PC_RING_HOT_EPOCHS takes
//      (default 32) covers it entirely with its extents (HotRingPages). Each take changes the mask
//      incrementally: HotRingDelta gives the pages that join and leave, and the copy side applies
//      only those (RegionManager::SetRingRange); the full mask is re-applied every 1024 submits and
//      when the arena blocks change. Verify stays on, so a stale byte shows as a divergence.
//   6  full ring, safe: protect then copy (K = 1, pc_ring_copy.cpp). The heap hook only queues the
//      whole pages of each arena allocation (EnterPush, lock-free: the game thread never touches the
//      buffer cache or the page protection). The draw thread applies the queued entries at the start
//      of each take (TakeRingEnters), after the take's extents are taken. A page is unprotected from
//      that take until the first take that covers it. That take re-protects the page, then copies
//      its bytes to VRAM; the ordering proof is at OnSubmitStart in pc_ring_copy.cpp. Only pages
//      covered entirely by one allocation enter the ring; every other page keeps normal write
//      protection. A full entry queue drops entries (counted). No verify.
//   7  mode 6 plus verify (CheckBind, as mode 4). Snapshots are taken after the protect step. A bind
//      does not compare a byte whose page is CPU-dirty at the bind (the bind uploads it; see
//      SetRingDirtyProbe) or whose page was uploaded after the snapshot (NoteUpload keeps a
//      per-page upload sequence). Both are counted apart. Expected divergence: 0.
//   8  host arena binds (BufferCache::PcHostBind, pc_ring_copy.cpp HostMaintain). Each arena block
//      is imported (SotcGuestMemory::ImportGuestRange, with the shader usages) and never copied:
//      a bind inside one imported run is served from the imported buffer, so the shaders read the
//      guest bytes in place (the PS4 unified-memory behaviour). The imported blocks are ringed
//      (never write-protected, never GPU-tracked); a bind that crosses a block edge uploads the
//      ringed bytes it covers (marked CPU-modified) and then takes the normal path. Writes by the
//      GPU into a served range go to host memory directly and are not tracked for readback.
//      Counted in the report line "SOTC PC RING host". Modes 3 to 7 are unchanged. Needs
//      SOTC_HOOK_ALLOC_PROBE=1 (the blocks); no heap hook is used.
//      Mode 8 details (design note: docs/re-stutters/PC-MEMORY-REWRITE.md section 8):
//      - GPU writes into served ranges land in guest memory with no fault and no GPU tracking, so
//        the CPU reads them as soon as the game reads them. The game's waits are EOP labels, and
//        the port writes an EOP label as soon as the command processor reads it, before the host
//        GPU runs the work, unless SOTC_EOP_TICK or SOTC_RB_EXACT defers it (both off by default).
//        So mode 8 defers an EOP label (liverpool.cpp, EventWriteEop) when a served write came since
//        the previous label of the queue, or a deferred label is still outstanding (labels land in
//        order). A deferred label lands when the GPU has finished the work recorded before it.
//        Read-protecting the written pages instead costs a protect call per written bind (about 34k
//        per 5 s) and a GPU wait on every read fault.
//        Not covered: ReleaseMem and EventWriteEos labels. Open until a log shows the game reading
//        served data behind one of them.
//      - SOTC_PC_RING8_VERIFY=1 (diagnostic): one bind in SOTC_PC_RING8_VERIFY_SAMPLE (default 32) is
//        hashed at bind time and again at the start of the next submit. A changed hash is a CPU
//        write between the bind and the next submit (the bind did not see it). Written binds are
//        rechecked the same way: a change there means the GPU already ran the submit.
//      - Adjacent imported blocks (contiguous in guest address space) are imported as one range, so
//        a bind across their edge is served too. A straddle counter counts such binds.
//      - Flip wait (SOTC_PC_RING8_FLIPWAIT=1, default off; BufferCache::PcHostFlipWait). The game reuses
//        a heap half two frames after it was current, but the port's GPU may still read it: EOP labels
//        and the VO flip label are written when the command processor parses them. At each frame
//        boundary the render thread drains the command processor (WaitGpuIdle), then waits for the
//        tick sampled at the previous boundary for the half it reuses. Needs SOTC_HOOK_FRAME_FLIP=1.
//      - Dedupe of the normal-path marks is per draw-side submit (PcHostMarkCpu); SOTC_PC_RING8_DEDUPE=0
//        turns it off.
// The epoch is each submit (OnSubmit). Needs SOTC_HOOK_ALLOC_PROBE=1 (the arena blocks);
// SOTC_HOOK_FRAME_FLIP=1 adds the per-frame averages.
//
// Verify and coverage (modes 4, 5). SOTC_PC_RING_VERIFY_SAMPLE=N checks one bind in N (N >= 1).
// SOTC_PC_RING_HOT_EPOCHS=K sets the hot window (1 to 64 takes).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SotcPcRing {

/// An arena range (guest address and size). Modes 3 and 4.
struct RingExtent {
    uint64_t addr;
    uint64_t size;
};

int Mode(); ///< SOTC_PC_RING, read once: 0 to 8
/// The full ring mode: 3 to 7, or 0 when the mode is not a full ring mode.
int FullRingMode();

/// Registers the PC_RING_ALLOC handler (hook_handlers.cpp).
void Register();
inline bool On() {
    return Mode() != 0;
}

/// Allocator probe feed: a block of an allocator object (name, kind). Only the kind-2
/// "Default TaggedArena" blocks are kept. Any thread; cheap when off.
void OnBlock(uint64_t base, uint64_t size, const char* name, int kind);
void OnFreeBlock(uint64_t base);

/// The frame boundary (MEMTYPE_FLIP_SITE, entry of sub_8c860). Render thread. Counts frames only.
void OnFrameBoundary();

/// A submit (gnmdriver PerformSubmit, or a compute doorbell), under the submit lock, before the
/// command processor sees the work. This is the ring's epoch: the game submits 16-20 times per
/// frame, and the draw thread binds each submit's data before the frame boundary (first shadow
/// run, 1 Nov: ~100 early binds and ~8 late writes per frame with a per-frame epoch).
void OnSubmit();

/// Guest write fault at addr (page_manager.cpp, write branch). Lock-free.
void NoteWriteFault(uint64_t addr);

/// A CPU range collected for upload by a bind (BufferCache::SynchronizeMemoryFull), with the bound
/// range it belongs to. Lock-free.
void NoteUpload(uint64_t addr, uint64_t size, uint64_t bind_addr, uint64_t bind_size);

/// Marks the uploads of the current thread as DMA sweep uploads (BufferCache::SynchronizeDmaBuffers,
/// whole resident runs) for as long as it lives, so they are counted apart from descriptor binds.
struct SweepScope {
    SweepScope();
    ~SweepScope();
    SweepScope(const SweepScope&) = delete;
    SweepScope& operator=(const SweepScope&) = delete;
};

/// Modes 3 and 4. Draw thread, at the start of processing a submit: appends the extents allocated
/// since the previous call (every epoch up to the current one) to out. Whole-block fallbacks are
/// appended after the recorded extents. Lock: a short spinlock shared with the heap hook.
void TakeSubmitExtents(std::vector<RingExtent>& out);

/// Modes 3 and 4. The live arena blocks, and a version that changes whenever the set of blocks
/// (or an active flag) changes. Draw thread; takes the block mutex briefly.
std::vector<RingExtent> ArenaBlocks(uint64_t& version);

/// Modes 3 and 4. True when [addr, addr + size) overlaps a live arena block. Lock-free.
bool InArena(uint64_t addr, uint64_t size);

/// Modes 4 and 5. Called by the draw thread right after TakeSubmitExtents with the extents it
/// returned: copies their readable bytes into the snapshot of this take (the last 4 takes are kept;
/// each snapshot is capped at 32 MiB, the rest is counted as skipped). Other modes: no-op.
void SnapshotExtents(const std::vector<RingExtent>& extents);

/// Modes 4 and 5. Draw thread, at a bind of [addr, addr + size). Only one bind in
/// SOTC_PC_RING_VERIFY_SAMPLE is checked (the first, the N+1th, ... by call count). For a checked
/// bind, each byte of the range is compared with the newest snapshot covering that byte; the
/// differing bytes are divergent, bytes no snapshot covers are counted apart. The first 16
/// divergent binds are logged to stderr with their address, size, snapshot age (submits), and the
/// first differing offset. The checked bind's arena bytes are also classified by coverage (see
/// Report). Other modes: no-op.
void CheckBind(uint64_t addr, uint64_t size);

/// Mode 5. Draw thread, after the take's history is recorded. The hot mask: the union, over the last
/// SOTC_PC_RING_HOT_EPOCHS takes, of the whole 4 KiB pages that one take covers entirely with its
/// extents, as sorted, disjoint extents (out is replaced). Pages only partly covered, or covered by
/// two takes without one of them covering the page, are not in the mask and keep normal write
/// protection. version is bumped whenever the returned set differs from the previous call's. The
/// set is checked against the state HotRingDelta keeps (g_hot_mismatch, must stay 0). Other modes:
/// out is empty.
void HotRingPages(std::vector<RingExtent>& out, uint64_t& version);

/// Mode 5, incremental. Draw thread, once per take, right after TakeSubmitExtents. Advances the hot
/// window by that take and returns the change of the hot mask: enter = pages that join it, leave =
/// pages that leave it (both sorted, disjoint, page aligned, never both for one page). Applying them
/// to the previous mask gives exactly the mask HotRingPages returns. Other modes: both empty.
void HotRingDelta(std::vector<RingExtent>& enter, std::vector<RingExtent>& leave);

/// Mode 5. Draw thread: the time of one delta apply (the protection changes), for the report.
void NoteHotDeltaApply(uint64_t ns);

/// Coverage input (modes 4 and 5). A GPU write of [addr, addr + size) inside the arena, stamped with
/// the current submit epoch. Any thread, lock-free. Not wired yet: the buffer cache's GPU-modified
/// marking would call it; until then the GPU-written coverage counters stay 0. Other modes: no-op.
void NoteGpuWrite(uint64_t addr, uint64_t size);

/// Modes 3 and 4. A copy of an extent's bytes to the GPU (imported: the guest memory was imported
/// as host memory and the GPU copies it; false: fallback through the CPU). Any thread.
void NoteRingCopy(uint64_t bytes, bool imported);

/// Modes 3 and 4. A guest write fault that the ring should have prevented (must stay 0). Any thread.
void NoteRingFault();

/// Modes 6 and 7. Draw thread, once per take, right after TakeSubmitExtents (never before it: the
/// order is the proof's). Appends the entries queued by the heap hook since the last call: the whole
/// pages [addr, addr + size) that one allocation covers entirely, in allocation order. Each entry
/// was queued before its extent was recorded, so every extent the take returned has its entry here
/// (or in an earlier call). The caller ring-enters them. Other modes: no-op.
void TakeRingEnters(std::vector<RingExtent>& out);

/// Mode 7. Callback (draw thread, at a bind): true when a CPU bit is set anywhere in [addr, addr +
/// size). The bind uploads such a page, so verify does not compare it. nullptr: no skip.
using RingDirtyFn = bool (*)(uint64_t addr, uint64_t size);
void SetRingDirtyProbe(RingDirtyFn fn);

/// Modes 6 and 7. The pages covered by the take of one submit, which leave the ring (protect then
/// copy). Draw thread.
void NoteRingLeave(uint64_t pages);

/// Mode 8. Relation of [addr, addr + size) to the live arena blocks: 0 outside all of them, 1 inside
/// one, 2 crossing a block edge. Other modes: 0. Lock-free.
int ArenaRelation(uint64_t addr, uint64_t size);
/// Mode 8. Lock-free counters of the block set and of the submit epoch (OnSubmit), read by the
/// draw thread to decide when the host imports must be refreshed.
uint64_t BlockVersion();
uint32_t SubmitEpoch();
/// Mode 8. The imported set after a refresh (gauge), and one count per bind of the window.
void NoteHostBlocks(uint64_t count, uint64_t bytes);
void NoteHostBind(uint64_t bytes, bool written);
void NoteHostStraddle(bool served); ///< a bind across a block edge; served: it used the import
void NoteHostFallback();
/// Mode 8. Count of served written binds (any thread, lock-free). The label side compares it with
/// the value seen at the previous label of its queue.
uint64_t HostWriteSeq();
/// Mode 8. Draw thread, at each submit start: unique bytes (overlapping binds merged) of the read
/// and of the written served binds of the submit that ended.
/// Mode 8: a windowed read-only bind clipped to the end of its imported run (PcHostClipLimited).
void NoteHostClip();
void NoteHostUnique(uint64_t read_bytes, uint64_t write_bytes);
/// Mode 8. The normal-path CPU marking of a bind's ringed bytes: marked bytes uploaded at the bind,
/// and skipped bytes already marked in the same submit epoch (BufferCache::PcHostMarkCpu).
void NoteHostMark(uint64_t marked, uint64_t skipped);
/// Mode 8 label side: one EOP label, deferred to the GPU or written early (liverpool.cpp).
void NoteHostLabel(bool deferred);
/// Mode 8 label side (SOTC_PC_RING8_LABELS, default on; 0 disables): a label that would have been deferred but is never
/// deferred because the submit waits on it (lookahead); a CP wait that flushed the open buffer for
/// a pending label; a deferred label written by the 500 ms watchdog.
void NoteHostLabelExcluded();
void NoteHostCpWaitFlush();
void NoteHostLabelForced();
/// Mode 8 verify (SOTC_PC_RING8_VERIFY=1): on, and one bind in how many is sampled.
bool Ring8Verify();
uint32_t Ring8VerifySample();
/// Mode 8: SOTC_PC_RING8_GROUP=1 (off by default). Imports adjacent arena blocks as one range and
/// serves the straddling binds from it. Off: a straddling bind takes the normal upload path, as
/// before the group change (with it on, ~1200 binds per 5 s took the full path, 1 Nov).
bool Ring8Group();
/// Mode 8 verify: a sampled bind rechecked at the next submit start. bytes: the hashed part.
void NoteHostVerify(bool written, uint64_t bytes, bool changed);
/// Mode 8 flip wait (SOTC_PC_RING8_FLIPWAIT=1, default off). OnFrameBoundary (the game's
/// render thread, MEMTYPE_FLIP_SITE hook: needs SOTC_HOOK_FRAME_FLIP=1) calls the registered function
/// once per frame before the game reuses a heap half. SetFlipWait is called once, when the rasterizer
/// exists. Other modes: nothing is called.
using FlipWaitFn = void (*)(void* ctx);
void SetFlipWait(FlipWaitFn fn, void* ctx);
bool Ring8FlipWaitOn();
/// One flip wait: its time, whether it waited on a tick, and how many of the two halves hold a tick.
void NoteFlipWait(uint64_t ns, bool waited, unsigned halves);

/// SOTC FPS 5 s report line; resets the window.
void Report(std::string& out, double seconds);

} // namespace SotcPcRing
