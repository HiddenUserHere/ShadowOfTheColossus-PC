// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: placement by memory class, glue between the decision core (pc_pool_place.h) and the PC pool and
// its feeds (pc_pool*.cpp, probe_alloc.cpp, probe_memtype.cpp, buffer and texture cache). Design:
// docs/re-stutters/reports3/11-placement-by-class.md, sections 5 and 6.
//
// Switches (all off by default):
//   SOTC_PC_PLACE=0..3          off, observe, verify, apply (SotcPcPlace::Mode, also "off".."apply")
//   SOTC_PC_RECYCLE=0..2        off, quarantine (retire and count, never reuse), recycle (reuse)
//   SOTC_PC_RECYCLE_FRAMES=N    quarantine length in frames (default 120, open: to be measured)
//   SOTC_PC_PLACE_WINDOWS=spec  VA windows "base:size:CLASS;..." (ParseWindowSpec), base and size in C
//                               integer syntax (0x hex or decimal), CLASS one of CPU_ONLY, GPU_STATIC,
//                               GPU_DYNAMIC, GPU_WRITTEN. Unset or empty: AUTO, the windows are carved from the
//                               three pool sets at the first observed allocation (see AutoWindowPlan below).
//                               "off" or "none": no windows, so nothing is ever windowed.
//
// Stages:
//   observe (1): every block the pool hands out is kept with its owner and heap decision, the block ledger,
//                GPU bind and write fault evidence. Report gives the per-class table. Nothing is changed.
//   verify  (2): also predicts, per block, whether it sits in its placement class window and what Effective()
//                would publish untracked, with and without the window. Counts what would break (a CPU_ONLY
//                publish later GPU-bound, a CPU_ONLY publish written, a publish lost to the window).
//   apply   (3): ChooseWindow and WindowsFor tell the native writer which window a request should fit in, and
//                FitInWindow picks the free run inside it (pure, on the pre snapshot). PublishList returns the
//                CPU_ONLY blocks Effective() still trusts. The caller maps the list to SotcUntrack in place of
//                SotcMemClasses::ApplyRanges. The writer's side of the change is the diff in the stage 3 report.
//   recycle 1:   freed runs are retired (VaRecycler) and the reuses they would give are counted, never taken.
//   recycle 2:   TakeRecycled hands a retired run back; the caller wires it to reserve_range (diff).
//
// Thread safety: every public method takes the glue's own mutex and never calls back into the game, so it is
// safe to call with the pool lock held. The glue is inert (one bool test) when both switches are off.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "pc_pool.h"
#include "pc_pool_place.h"

namespace SotcPcPlaceGlue {

namespace PP = SotcPcPlace;

struct Config {
    PP::Mode place = PP::Mode::Off;
    PP::VaRecycler::Mode recycle = PP::VaRecycler::Mode::Off;
    uint64_t quarantine_frames = 120;
    std::string windows_spec;     ///< explicit windows (SOTC_PC_PLACE_WINDOWS), empty when none was given
    bool windows_auto = false;    ///< SOTC_PC_PLACE_WINDOWS unset or empty: carve the windows from the pool sets
    std::string warnings; ///< what MakeConfig and the Glue constructor refused, one sentence each
};

/// Pure parsing of the switch texts (null or empty means the default). Bad values fall back to the default and
/// append a sentence to cfg.warnings. windows: null or empty is auto; "off" or "none" is no windows; anything
/// else is an explicit spec (ParseWindowSpec).
Config MakeConfig(const char* place, const char* recycle, const char* frames, const char* windows);
/// Reads SOTC_PC_PLACE, SOTC_PC_RECYCLE, SOTC_PC_RECYCLE_FRAMES and SOTC_PC_PLACE_WINDOWS.
Config ConfigFromEnv();
/// Parses "base:size:CLASS;base:size:CLASS". Does not validate the windows (Glue does, through WindowMap::Set).
bool ParseWindowSpec(const char* spec, std::vector<PP::VaWindow>* out, std::string* why);

/// One pool set as the auto window plan sees it: its VA base and size, and whether it is pristine (no region, no
/// alloc, no backed bytes, the SeedMap test). A set that is not pristine gets no windows.
struct PoolSetLayout {
    uint64_t base = 0;
    uint64_t size = 0;
    bool pristine = true;
};
/// The default layout of the three sets (observed, reports3/06 section 1.1): S0 flexible, S1 Onion, S2 Garlic.
/// Pristine is assumed unless the pool reports otherwise through Glue::SetPoolLayout.
extern const PoolSetLayout kDefaultPoolSets[3];

/// Sets the heap slot of the calling thread for the duration of a 0x12820 call. OnAllocObserved uses it when its
/// own heap_slot is negative. Nests (the previous slot is restored).
class HeapScope {
public:
    explicit HeapScope(int32_t slot);
    ~HeapScope();
    HeapScope(const HeapScope&) = delete;
    HeapScope& operator=(const HeapScope&) = delete;

private:
    int32_t prev_;
};
/// The heap slot set by the innermost HeapScope of this thread, or -1.
int32_t CurrentHeapSlot();
/// The same as HeapScope for a hook that runs the pre and post phase separately: Push returns the previous slot, and
/// the post phase calls Pop with it.
int32_t PushHeapSlot(int32_t slot);
void PopHeapSlot(int32_t previous);

/// True when the placement glue is on (SOTC_PC_PLACE or SOTC_PC_RECYCLE set to a mode). The feed call sites test this
/// instead of SOTC_CLASS_TRACK: the glue has its own gate.
bool On();

/// A range that may be published untracked (CPU_ONLY through Effective), and its pool set (0, 1, 2).
struct PublishRange {
    uint64_t addr = 0;
    uint64_t size = 0;
    int set = -1;
};

/// Stage 3 fit: the free run of pre.set[set] that FitInWindow chose, the aligned block VA inside it, and whether the
/// native exact writer (ApplyExactHit) can apply it: no pad before the start and no split (see HitPath in pc_pool.cpp).
struct WindowFit {
    size_t run = 0;        ///< index in pre.set[set].free_va
    uint64_t start = 0;    ///< the block VA (aligned to the request's alignment, inside the window)
    bool exact = false;    ///< start == run.base and bytes >= run.backed
};

/// Stage 3, pure: the first free run of pre.set[set] (list order, the walk of pool_take_free_run) that holds an
/// aligned block of bytes entirely inside the window w. bytes must be a non-zero multiple of 64 KiB, align a power of
/// two. Returns false when the set has too few free VA pages, or no run meets the window. A run that only partly
/// overlaps the window is cut to the window first. Does not look at any other window: call it once per window of the
/// class (Glue::WindowsFor gives them in base order).
bool FitInWindow(const SotcPcPool::Model::Pools& pre, int set, const PP::VaWindow& w, uint64_t bytes, uint64_t align,
                 WindowFit* out);

class Glue {
public:
    explicit Glue(const Config& cfg);

    /// False when both switches are off: every call is a no-op.
    bool enabled() const { return enabled_; }
    PP::Mode place_mode() const { return cfg_.place; }
    PP::VaRecycler::Mode recycle_mode() const { return cfg_.recycle; }
    /// Refusals from MakeConfig and the constructor (windows rejected, bad switch values). Empty when none.
    std::string warnings() const { return cfg_.warnings; }

    /// Adds a decision rule (Addr, NameKind or Heap). Returns false with *why for a malformed rule.
    bool AddRule(const PP::Rule& rule, std::string* why);

    /// Pool set layout for the auto window plan. Call before the first observed allocation (the windows are carved
    /// once, at that allocation); a later call changes nothing. Without it kDefaultPoolSets is used.
    void SetPoolLayout(const PoolSetLayout sets[3]);

    /// A block the pool handed out: va, size bytes, pool set (0..2), and the owner object (addr, name, kind).
    /// heap_slot < 0 means "use the thread's HeapScope"; if that is also negative the call is counted as having no
    /// heap context (the verify stage's open question, design section 1.3). Call from the pool path, after the
    /// block is built and before the pool returns it.
    void OnAllocObserved(const PP::OwnerKey& owner, uint64_t va, uint64_t size, int set, int32_t heap_slot);
    /// A block the game freed (FREE_BLOCK, at entry). The record is dropped now. With recycling on, the run becomes
    /// pending and is retired at the next OnFlushDone, because the game unmaps it only in the batch flush. Only a
    /// block seen by OnAllocObserved with the same size is ever pending.
    void OnFreeObserved(uint64_t va, uint64_t size);
    /// The batch flush (vmem_batch_map) has run. is_unmapped(va, size) is the caller's PhysMap check: a pending run it
    /// confirms is retired, any other is refused (counted). Pending runs are consumed either way.
    void OnFlushDone(const std::function<bool(uint64_t va, uint64_t size)>& is_unmapped);
    /// A heap of heap_slot carved memory at addr (any address: the granule or the block). The glue finds the block
    /// that holds addr and feeds the BlockLedger; an addr in no known block is counted as untracked.
    void OnHeapCarve(uint64_t addr, int32_t heap_slot);

    /// GPU bind of [addr, addr + size): buffer or image bind (the same calls as SotcMemClasses::NoteGpuBind). Blocks it
    /// overlaps become GPU-bound, for good. Any thread: the glue's mutex orders the calls, and a bind from another
    /// thread can only make a block less CPU_ONLY.
    void OnGpuBind(uint64_t addr, uint64_t size);
    /// Guest write fault at addr (page_manager write branch). A write to a block predicted CPU_ONLY demotes it.
    void OnWriteFault(uint64_t addr);

    /// Frame of the command processor (the recycler's clock). Call once per frame, draw thread.
    void OnFrameBegin(uint64_t frame);
    /// The GPU finished every submit up to gpu_done_frame (gnmdriver PerformSubmit fence).
    void OnSubmitFence(uint64_t gpu_done_frame);
    /// The buffer and texture caches dropped every entry overlapping [va, va + size).
    void OnCachesInvalidated(uint64_t va, uint64_t size);

    /// Apply stage. The native writer asks where a request of this owner and heap should come from: returns true and
    /// the window (base aligned to align, size = the bytes left in the window from that base) when a window of the
    /// placement class fits size. False in every other mode, or when no window fits.
    bool ChooseWindow(const PP::OwnerKey& owner, int32_t heap_slot, uint64_t size, uint64_t align, PP::VaWindow* out);
    /// Apply stage: every window of the placement class of this request, in base order (empty in every other mode, or
    /// when the placement is Unknown). The native writer tries FitInWindow on each one against its pre snapshot.
    std::vector<PP::VaWindow> WindowsFor(const PP::OwnerKey& owner, int32_t heap_slot);
    /// The windows in use (explicit or auto), in base order.
    std::vector<PP::VaWindow> Windows();
    /// Recycle 2 (Quarantine: counted, false). Returns a retired run of class cls for a fresh block (va_out).
    bool TakeRecycled(PP::Class cls, uint64_t size, uint64_t align, uint64_t* va_out, std::string* why);

    /// Apply stage: the ranges that may be published untracked now (CPU_ONLY through Effective with the windows). Empty
    /// in the other modes. *version bumps whenever the returned list differs from the previous call's.
    std::vector<PublishRange> PublishList(uint64_t* version);

    /// Appends the report lines (prefix "SOTC PC PLACE" and "SOTC PC RECYCLE"). Empty when both switches are off.
    /// seconds gives the rates of the binds and write faults since the previous report.
    void Report(std::string& out, double seconds);

    size_t blocks() const;

private:
    struct Block {
        uint64_t size = 0;
        int set = -1;
        PP::Decision decision;
        bool bound = false;   ///< a GPU bind overlapped the block (sticky)
        bool demoted = false; ///< a write landed on the block while it was predicted CPU_ONLY (sticky)
    };
    struct Counters {
        uint64_t allocs = 0;
        uint64_t alloc_bytes = 0;
        uint64_t dup_allocs = 0;
        uint64_t no_heap_context = 0;
        uint64_t frees = 0;
        uint64_t frees_unobserved = 0;
        uint64_t frees_size_mismatch = 0;
        uint64_t carves = 0;
        uint64_t carves_unruled = 0;
        uint64_t carves_untracked = 0;
        uint64_t binds = 0;
        uint64_t binds_untracked = 0;
        uint64_t bound_blocks = 0;
        uint64_t cpu_only_bound = 0;          ///< a bind hit a block predicted CPU_ONLY (windowless prediction)
        uint64_t cpu_only_bound_windowed = 0; ///< the same, with the window check
        uint64_t write_faults = 0;
        uint64_t write_faults_untracked = 0;
        uint64_t cpu_only_write = 0;          ///< a write hit a block predicted CPU_ONLY: demoted
        uint64_t choose_hits = 0;
        uint64_t choose_misses = 0;
        uint64_t takes = 0;
        uint64_t takes_hit = 0;
    };

    void EnsureWindows();
    void ReportPlace(std::string& out, double secs);
    void ReportWindows(std::string& out);
    void ReportRecycle(std::string& out);
    PP::Class Publish(const Block& b, uint64_t base, bool use_window) const;
    PP::Class WindowClassOfBlock(uint64_t base, uint64_t size) const;
    bool LedgerOk(uint64_t base, PP::Class publish) const;
    Block* FindBlock(uint64_t va, uint64_t* base_out);

    mutable std::mutex mu_;
    Config cfg_;
    bool enabled_ = false;
    PP::ClassTable rules_;
    PP::WindowMap windows_;
    bool windows_done_ = false;      ///< the windows are final (explicit, auto carved, or auto off)
    std::string windows_note_;       ///< auto plan: what was carved per set, and what was skipped and why
    PoolSetLayout layout_[3] = {kDefaultPoolSets[0], kDefaultPoolSets[1], kDefaultPoolSets[2]};
    PP::BlockLedger ledger_;
    PP::VaRecycler recycler_;
    std::map<uint64_t, Block> blocks_; ///< block VA -> block (blocks never overlap)
    Counters c_;
    Counters last_report_;
    uint64_t frame_ = 0;
    uint64_t gpu_done_ = 0;
    uint64_t version_ = 0;
    std::vector<PublishRange> published_;
    uint64_t last_publish_bytes_ = 0;
    struct Pending {
        uint64_t va = 0;
        uint64_t size = 0;
        PP::Class cls = PP::Class::Unknown;
    };
    std::vector<Pending> pending_; ///< freed runs waiting for the batch flush
};

/// The process-wide glue, built from the environment on first use.
Glue& Global();

} // namespace SotcPcPlaceGlue
