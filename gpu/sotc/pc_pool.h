// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PcPool verification model, SOTC_PC_POOL=2 (docs/re-stutters/PC-MEMORY-REWRITE.md section 2;
// the game's pool layer is specified in docs/re-stutters/reports3/06-pool-layer-spec.md, corrected
// here from the disassembly of pool_alloc_region 0x19e00, pool_take_free_run 0x1a800, vmem_query_reserve_map
// 0x1abf0 and pool_free_block 0x1b0e0).
//
// Phase 1 of the PC memory rewrite replaces the game's pool layer with a port allocator. This file
// holds the verification step only: before each block allocation the model reads the pool root state,
// predicts the result of the game's own pool_alloc_region (0x19e00), and compares the prediction with
// what the game returned and left in memory. It changes nothing in the game.
//
// Hooks (scripts/hooks.d/pcpool.py, switch SOTC_PC_POOL):
//   PC_POOL_ALLOC  wrap at pool_alloc_block_wrapper (0x19ae0), the caller of 0x19e00 on the block path.
//                  Observe only. 0x19e00 itself cannot be hooked (unresolved indirect branch).
//   PC_POOL_MAP    wrap at vmem_batch_map (0x1af80), the flush of the pool's op queue. Every VA ->
//                  phys change of the game goes through it, so its entries feed the shadow VA map
//                  (PhysMap). Its own group: a refusal of either hook does not turn the other off.
//   Direct and boot callers (scripts/hooks.d/pcpool.py): PC_POOL_SIMPLE wrap at 0x19c60; PC_POOL_ALIGNED entry at
//                  0x1a660 with PC_POOL_ALIGNED_EXIT site at its normal exit (0x1a7db); PC_POOL_BPE_INIT (0x18aa0) and
//                  PC_POOL_FLEX_INIT (0x197d0) entries. The direct ones verify under SOTC_PC_POOL=2 and announce their
//                  source under SOTC_PC_POOL_NATIVE=1; the boot ones only re-base. Counted per source in the report.
//   PC_POOL_NATIVE entry at pool_alloc_block_wrapper (0x19ae0), switch SOTC_PC_POOL_NATIVE=1. Runs the
//                  eligible calls natively (SotcPcPoolNative::ApplyExactHit), performs the wrapper's tail and
//                  returns the node; the wrapper does not run for them. See Model::Native.
//   SOTC_PC_POOL=2 (verify, PC_POOL_ALLOC wrap) and SOTC_PC_POOL_NATIVE=1 at the same time: both alloc
//                  hooks are inert and one line says so. SOTC_PC_POOL=1 is not a mode (one log line, off).
// Frees are not hooked here (FREE_BLOCK in scripts/hooks.d/alloc.py already hooks 0x1b0e0 at entry).
// The lead calls OnFreeNode(node) from that handler, so frees are checked as accounting deltas.
//
// The Model namespace is pure (no guest access) and is what tests/pc_pool_test.cpp exercises.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace SotcPcPool {

/// Registers the PC_POOL_ALLOC handlers. Called from RegisterAllHandlers (hook_handlers.cpp).
void Register();

/// Appends the verification report (cumulative totals) to out. Prints nothing when SOTC_PC_POOL is off.
/// seconds is accepted for the driver's report signature and is not used.
void Report(std::string& out, double seconds);

/// Called by the lead's FREE_BLOCK handler (probe_alloc.cpp) with the node that pool_free_block
/// (0x1b0e0) is called with. It reads the node's pool id (+0x68), size (+0x58) and backed bytes (+0x50).
void OnFreeNode(uint64_t node);

// sotc: the windows of the native path (pc_pool_windows.h, allocators.md section 6). Native mode only (SOTC_NATIVE=1 or
// 2 with SOTC_PC_POOL_NATIVE=1); every function returns at once otherwise, so the hooks that call them are inert in
// mode 0. Hook sites are in docs/re-stutters/native/impl-windows.md.
/// After the arena creation (0x110a0): reserves window A (GPU arena) and window B (mask heap) and binds them to the two
/// arena objects (globals 0x2dfb2b8 and 0x2dfb2c0). Once per session.
void SetupWindows();
/// At the render-target allocator entry (0x9b778) with its object: reserves window C once and binds it to that object.
void BindRenderTargets(uint64_t rt_obj);
/// The heap-slot scope (dispatcher 0x12820): the slot the next block of this thread is for. Entry sets, exit clears.
void SetHeapKey(int64_t key);
void ClearHeapKey();
/// The release of a block (0x476f0 entry, with its owner and node): when the node is a parked window block, does
/// the unlink, the count and the bytes of 0x476f0 and skips the VA return 0x1b0e0. Returns true when it did (the hook
/// then returns without the original). False: nothing written, the original runs.
bool WindowRelease(uint64_t owner, uint64_t node);
/// The allocation of a block (0x479a0 entry, with its owner and rounded size): when a parked node of that owner and
/// size exists, inserts it into the owner's list as 0x479a0 does and returns its node in *node. Returns true when
/// it did (the hook returns that node). False: the original runs.
bool WindowReuse(uint64_t owner, uint64_t size, uint64_t* node);

namespace Model {

/// The pool granule: every run, node size and alignment is a multiple of 64 KiB.
constexpr uint64_t kPageBytes = 0x10000;
/// The S1/S2 search alignment is rounded up to this (pool_alloc_region, uVar17 = roundup(align, 1 MiB)).
constexpr uint64_t kMiB = 0x100000;
/// Index = pool id (+0x78): 0 flexible (VA only), 1 Onion, 2 Garlic.
constexpr int kSetCount = 3;

/// A free-VA run, or an allocated block as the game keeps it (node +0x60 base, +0x58 size, +0x50 backed).
struct Run {
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t backed = 0; ///< bytes of the run that the game counts as backed (node +0x50)
    uint64_t node = 0;   ///< guest address of the node (not compared; the native write check uses it)
    bool operator==(const Run& o) const { return base == o.base && size == o.size && backed == o.backed; }
};

/// One pool set, as the game stores it at root, root+0xa0 and root+0x140.
struct Set {
    int id = 0;                    ///< +0x78 pool id
    bool has_bitmap = false;       ///< the flexible set (id 0) has no phys bitmap
    int32_t region_count = 0;      ///< +0x20
    int32_t free_va_pages = 0;     ///< +0x70: decremented by a block taken from the free-VA list and by a
                                   ///<        reserve-map (vmem_query_reserve_map flag 1); +pages on a free into S0
    int32_t cached_phys = 0;       ///< +0x74 free phys pages counted in the bitmap
    int32_t stored_free_count = 0; ///< +0x48 free-VA node count (compared with the list length)
    uint64_t alloc_bytes = 0;      ///< +0x80
    uint64_t backed_bytes = 0;     ///< +0x90
    uint32_t page_cap = 0;         ///< +0x60: an index bound, not a run length. The scan ignores bits at or
                                   ///<        above it (0x1a800/0x1a3xx: `cap <= first_bit` -> none). Set to
                                   ///<        the bitmap size in pages for an unbounded bitmap.
    std::vector<Run> free_va;      ///< free-VA runs in list (address) order, from the walk of +0x38
    std::vector<uint64_t> words;   ///< phys bitmap, bit i = phys 0x100000 + i*64 KiB, bit 1 = free
    uint64_t words_addr = 0;       ///< guest address of the bitmap words (+0x50; not compared)
};

struct Pools {
    Set set[kSetCount];
};

/// The arguments of pool_alloc_region (0x19e00) as the wrapper passes them: va_hint = 0, node = 0, mode = 0
/// (the only path this model covers). size = rcx, align = r9d, kind = the stack argument.
struct Request {
    uint64_t va_hint = 0;
    uint64_t node = 0;
    uint64_t size = 0;
    uint32_t mode = 0;
    uint64_t align = 0;
    int32_t kind = 0;
    uint64_t obj = 0; ///< rdi, the owner / family object. Not modelled; the native application reads it (node+0x40)
    /// sotc: a window route (pc_pool_windows.h). The VA is not taken from a free-VA run (steps 1 and 3 of Predict are
    /// skipped), so the call is a fresh block whose VA the window gives. Early reject and the page loop are unchanged.
    bool force_fresh = false;
};

enum class Outcome {
    Predicted,  ///< a block with the predicted pool id, and (for a fresh block) an unknown VA
    Null,       ///< the early reject: the game should return 0 and change nothing
    WouldCrash, ///< the game would read a null or past-the-end node (spec 2.5 and the reserve-map path)
    Unmodelled, ///< a path the model does not cover; reason says which
};

/// The prediction path of one call, for the per-path counters (SOTC_PC_POOL_PATHS) and the native eligibility.
/// A hit that pads or splits its free run takes a node from pool_node_alloc (0x19660). That routine pops the
/// node pool at root+0x1f0 and, when the count drops from 3 to 2, refills it through a nested pool_alloc_region
/// (0x19e00) that does not pass through the wrapper, so the hooks do not see it. The *Node hits are therefore
/// not certain; the *Exact hits take no node.
enum class Path : uint8_t {
    FlexHitExact,    ///< kind 1, first fit in S0, no pad and no split
    FlexHitNode,     ///< kind 1, first fit in S0 with a pad or a split (one node from pool_node_alloc)
    PrimaryHitExact, ///< first fit in the primary set (S1 for kind 1, S2 otherwise), no pad and no split
    PrimaryHitNode,  ///< first fit in the primary set with a pad or a split
    FreshPlain,      ///< fresh VA; the page loop takes bitmap bits of the primary set only
    FreshReserve,    ///< fresh VA; the page loop ran vmem_query_reserve_map (cached count was zero)
    FreshSpill,      ///< fresh VA; the page loop continued in the spill set
    FreshPoolFix,    ///< fresh VA; the POOL_FIX path (no free bit, cached count zeroed)
    EarlyReject,     ///< the early reject: returns 0, changes nothing
    WouldCrash,      ///< the game would crash here (null node)
    Unmodelled,      ///< not covered by the model
    kCount
};
constexpr int kPathCount = static_cast<int>(Path::kCount);
const char* PathName(Path path);
constexpr uint32_t PathBit(Path path) { return uint32_t{1} << static_cast<int>(path); }
/// Parses a SOTC_PC_POOL_PATHS value: comma-separated path names (PathName), "all" or "none". Names that do
/// not match are reported in *bad (the bits of the others are still set).
uint32_t ParsePathMask(const char* spec, std::string* bad);
/// The default eligible set: the exact free-run hits only. They take no node and change no bitmap and no batch
/// op. Not yet verified against the game: a run reports the matches and mismatches of each path.
constexpr uint32_t kDefaultPathMask = PathBit(Path::FlexHitExact) | PathBit(Path::PrimaryHitExact);
/// sotc: the fresh paths, eligible for a window route (Request::force_fresh) whatever SOTC_PC_POOL_PATHS says.
constexpr uint32_t kFreshPathMask = PathBit(Path::FreshPlain) | PathBit(Path::FreshReserve) |
                                    PathBit(Path::FreshSpill) | PathBit(Path::FreshPoolFix);

/// One phys run the game maps: bits [first_page, first_page + pages) of the set's bitmap, mapped at va.
struct PhysRun {
    int set = 0;
    uint64_t first_page = 0;
    uint64_t pages = 0;
    uint64_t va = 0;       ///< 0 for a fresh block (its VA is only known from the game)
    int protect_type = -1; ///< op 4 type when the run came from the spill set (-1: no op 4)
};

/// The shadow VA -> phys map of the game's pool memory: which 64 KiB VA pages the game has mapped, and to
/// which phys page (bit index). Bit b is phys 0x100000 + b * 64 KiB for both the Onion and the Garlic
/// bitmap (vmem_query_reserve_map and the page loop of pool_alloc_region add the same base; S2's pages
/// start at bit 0x4000). Its source is the game's batch queue: vmem_batch_map (0x1af80) applies every
/// op 0 (map), op 1 (unmap) and op 4 (protect type, no change of the mapping) entry.
///
/// Two kinds of knowledge are kept apart. A known run is a mapping (or an unmapping) the model has seen.
/// An unknown range is a VA range whose state the model could not observe (mapped before the hook was
/// installed, or touched by a batch that failed: the game skips the failing entry and the model cannot tell
/// which). A page in neither is unmapped. Any op that changes the mapping and that the model does not
/// understand marks its range unknown.
class PhysMap {
public:
    enum class State { Unmapped, Mapped, Unknown };

    /// The state of the 64 KiB VA page that contains va. On Mapped, *bit gets its phys bit index.
    State Lookup(uint64_t va, uint64_t* bit) const;
    /// Maps [va, va + bytes) to the phys bits [bit, bit + bytes / 64 KiB). Replaces what was there.
    void Map(uint64_t va, uint64_t bytes, uint64_t bit);
    /// Unmaps [va, va + bytes): known unmapped afterwards (an unknown range there becomes known).
    void Unmap(uint64_t va, uint64_t bytes);
    /// Marks [va, va + bytes) as state unknown.
    void MarkUnknown(uint64_t va, uint64_t bytes);
    void Clear();
    size_t MappedRuns() const { return mapped_.size(); }
    size_t UnknownRuns() const { return unknown_.size(); }

private:
    struct MapRun {
        uint64_t bytes = 0;
        uint64_t bit = 0; ///< phys bit of the first page of the run
    };
    std::map<uint64_t, MapRun> mapped_;  ///< VA start -> run
    std::map<uint64_t, uint64_t> unknown_; ///< VA start -> VA end (exclusive)
    void Subtract(uint64_t va, uint64_t bytes); ///< removes known and unknown coverage of [va, va + bytes)
};

/// The phys base of bit 0 as the bpe init sets it (global 0x2dfca70): bit b is phys base + b * 64 KiB, the
/// batch op offset of a page. The port reads the global at each flush; this is its usual value.
constexpr uint64_t kPhysBaseOffset = 0x100000;

/// Seeds the map at the first observation (pools == nullptr: a batch flush came first, and the map is taken
/// as empty, since every mapping of pool memory goes through the batch queue). With a snapshot, the map starts
/// empty when S1 and S2 are pristine (no region, alloc or backed bytes); otherwise the free-VA runs of S1 and
/// S2 are marked unknown, because their mapping predates the hook. Returns true when they were marked.
bool SeedMap(PhysMap* map, const Pools* pools);

/// Applies one op of the game's batch queue (vmem_batch_map, spec 2.4) to the map: op 0 maps [va, va+length) to
/// the phys offset, op 1 unmaps, op 2 and op 4 change protection or type only. op 3 and any other op are not
/// tracked and make their range unknown. failed: the flush returned an error. The game skips the failing entry
/// and the model cannot tell which entries took effect, so every mapping op of such a flush is unknown.
void ApplyBatchOp(PhysMap* map, uint64_t va, uint64_t offset, uint64_t length, int32_t op, bool failed,
                  uint64_t phys_base = kPhysBaseOffset);

struct Prediction {
    Outcome outcome = Outcome::Unmodelled;
    Path path = Path::Unmodelled;
    const char* reason = "";
    int slot = -1;               ///< pool id of the block (node +0x68)
    uint64_t va = 0;             ///< the block VA when it came from a free-VA run (fresh: 0)
    uint64_t size = 0;           ///< block size in bytes (rounded to 64 KiB)
    bool fresh = false;          ///< the VA came from a new sceKernelReserveVirtualRange (not predicted)
    uint64_t va_align = 0;       ///< fresh blocks: the VA must be a multiple of this
    std::vector<PhysRun> runs;   ///< phys runs taken by the page loop (the game's batch map ops)
    std::vector<Run> unmapped;   ///< VA ranges the reserve-map path unmaps (their map entries go away)
    /// Unmodelled because the reserve-map path reached a VA range whose state the map does not know
    /// (counted apart from the other Unmodelled reasons).
    bool unknown_state = false;
};

/// Predicts one normal allocation (mode 0, no VA hint, no node) from the state before the call.
/// On Predicted and Null, *post receives the state the game should leave. Pure: no guest access.
/// map is the VA->phys map as it is before the call (null: the reserve-map path is Unmodelled).
Prediction Predict(const Pools& pre, const Request& req, const PhysMap* map, Pools* post);

/// True for the four paths that take a fresh block (the page loop, no node reuse): FreshPlain, FreshReserve,
/// FreshSpill, FreshPoolFix. Every one of them pops a node from pool_node_alloc (0x19660) inside the call (the
/// fresh node of pool_alloc_region, 0x19fbf).
bool IsFreshPath(Path path);

/// True when the node pop of a fresh call refills the node pool: the pop takes the count from 3 to 2 and
/// pool_node_alloc then calls pool_alloc_region (0x19e00) for a 64 KiB block. The refill changes the sets, and the
/// model does not predict it on fresh paths (PredictNodeCall covers the two hit paths only). The count is the
/// root+0x200 value before the call.
bool FreshPopRefills(Path path, int32_t node_count_pre);

/// True when a call may be performed natively under the path mask: the prediction is Predicted or Null, the path is
/// in mask, and the path is neither Unmodelled nor WouldCrash (those never run natively, whatever the mask says).
bool Eligible(const Prediction& p, uint32_t mask);

/// Field bits of a mismatch. Bit i is named by FieldName(i).
enum Field : uint32_t {
    kFieldNode = 1u << 0,
    kFieldVa = 1u << 1,
    kFieldSize = 1u << 2,
    kFieldSlot = 1u << 3,
    kFieldFreeVa = 1u << 4,
    kFieldFreeCount = 1u << 5,
    kFieldFreeVaPages = 1u << 6,
    kFieldCached = 1u << 7,
    kFieldRegion = 1u << 8,
    kFieldAlloc = 1u << 9,
    kFieldBacked = 1u << 10,
    kFieldBitmap = 1u << 11,
};
constexpr int kFieldCount = 12;
const char* FieldName(int index);

/// What the game returned and left in memory after the call.
struct Observed {
    uint64_t node = 0; ///< rax
    uint64_t va = 0;   ///< node +0x60
    uint64_t size = 0; ///< node +0x58
    int slot = -1;     ///< node +0x68
    Pools post;
};

/// True when two sets have the same guest-visible state (the fields the model predicts).
bool SameSet(const Set& a, const Set& b);

/// Compares the observed post state with the prediction. Sets the model changes (predicted post differs from
/// pre) are compared field by field. A set the model leaves untouched is not compared: a change there is
/// counted in *outside_sets (a nested or unhooked allocation), not as a mismatch. Returns the mismatch bits;
/// details (optional) gets one line per field.
uint32_t Compare(const Prediction& p, const Pools& pre, const Pools& predicted, const Observed& got,
                 std::string* details, int* outside_sets);

/// Accumulated accounting of the frees seen since the last checkpoint, per pool id.
struct FreeAcc {
    int64_t frees = 0;
    int64_t region = 0;
    int64_t alloc = 0;
    int64_t backed = 0;
    int64_t free_va_pages = 0; ///< expected change of +0x70 (0 for Onion/Garlic: the VA is consumed, see AddFree)
    int64_t bits = 0;          ///< expected popcount change of the bitmap (mapped pages; 0 for the flexible set)
    int64_t cached = 0;        ///< expected change of +0x74 (the same pages, returned to the bitmap)
};

/// Adds one free of a block (pool id slot, size and backed bytes) to acc[slot]: spec 2.3, traced in the
/// disassembly of pool_free_block 0x1b0e0 and vmem_query_reserve_map 0x1abf0 (flag 1).
void AddFree(FreeAcc* acc, int slot, uint64_t size, uint64_t backed);

/// Checks the accounting of the frees in acc between two states of the same set. Returns the mismatch
/// bits (kFieldRegion, kFieldAlloc, kFieldBacked, kFieldBitmap, kFieldCached, kFieldFreeVaPages).
/// *va_delta gets the observed change of free_va_pages.
uint32_t CheckFree(const Set& before, const Set& after, const FreeAcc& acc, int64_t* va_delta,
                   std::string* details);

// ---- native execution (SOTC_PC_POOL_NATIVE=1, hook PC_POOL_NATIVE) ----------------------------------
// The eligible calls run natively: the entry hook at 0x19ae0 takes the game's pool lock, predicts the call
// from the snapshot, performs it through SotcPcPoolNative::ApplyExactHit, checks the result against the
// prediction and returns the node. Any failed check kills native execution for the session. The decision
// lives here (pure, testable); the glue in pc_pool.cpp implements Host on the game's memory.
namespace Native {

/// What the glue provides. Every method is called with the game's pool lock held, except TakeLock.
class Host {
public:
    virtual ~Host() = default;
    /// Takes the game's pool lock (bounded spin). false: not taken, nothing to release.
    virtual bool TakeLock() = 0;
    virtual void ReleaseLock() = 0;
    /// The three sets as they are now.
    virtual bool Snapshot(Pools* out) = 0;
    /// Predicts the call from pre. The glue passes the shadow VA map (kept under SOTC_PC_POOL_NATIVE=1 by
    /// PC_POOL_MAP_NATIVE) when that hook is installed; without it the reserve-map path is Unmodelled. Exact hits
    /// do not use the map.
    virtual void Predict(const Pools& pre, const Request& req, Prediction* p, Pools* predicted) = 0;
    /// Performs the predicted call natively, through the writer of p.path (the name is historical: the glue
    /// dispatches by path). Returns the node, or 0 when it refused having written nothing.
    virtual uint64_t ApplyExactHit(const Pools& pre, const Prediction& p, const Request& req) = 0;
    /// The guest ranges the last ApplyExactHit wrote.
    virtual std::vector<std::pair<uint64_t, size_t>> LastWriteSet() = 0;
    /// True when [addr, addr + n) lies in memory the model owns in pre: the three set headers, the free-VA
    /// nodes of pre and the bitmap words of pre.
    virtual bool InModelledMemory(const Pools& pre, uint64_t addr, size_t n) = 0;
    /// sotc: diagnostic text for a kill (the writer that ran, its touched nodes); "" by default.
    virtual std::string KillContext() { return {}; }
    /// Reads the node's VA, size and slot into got (got->node is set by the caller) and the post state into
    /// got->post.
    virtual bool Observe(uint64_t node, Observed* got) = 0;
    /// The wrapper's tail after the call (its peak counters), before the lock is released.
    virtual void Tail() = 0;
    virtual void Log(const std::string& text) = 0;
    /// Why the last ApplyExactHit refused (a writer's own reason name, "" when the writer gives none). Read only
    /// after a refusal; the controller counts it per predicted path (Stats::refused_reason).
    virtual const char* RefusalReason() { return ""; }
};

/// The route of one call: Native means the call ran natively and the game's state is already changed (the
/// node is the wrapper's return value); Original means the game's wrapper runs and the controller changed nothing.
enum class Route : uint8_t { Native, Original };

/// Why a call ran the original. A native call that fails its check is a kill, not a fallback.
enum class Fallback : uint8_t { LockBusy, SnapshotFailed, Ineligible, Refused, Killed, kCount };
constexpr int kFallbackCount = static_cast<int>(Fallback::kCount);
const char* FallbackName(Fallback fallback);

struct Stats {
    uint64_t calls = 0;          ///< calls seen by the hook
    uint64_t native_calls = 0;   ///< calls that ran natively (the state changed through ApplyExactHit)
    uint64_t verified = 0;       ///< native calls whose post state matched the prediction
    uint64_t kills = 0;          ///< 0 or 1: native execution was turned off for the session
    uint64_t checks_full = 0;    ///< periodic re-snapshots compared with the running expectation
    uint64_t checks_skipped = 0; ///< periodic re-snapshots with no valid expectation (a free or an original call since)
    uint64_t periodic_outside = 0; ///< periodic checks that found changes made outside native calls (the goal: 0)
    /// Expectation invalidations keyed by the source that announced the change (NoteUnseenChange). A source that
    /// is hooked and announced never shows up as periodic_outside.
    std::map<std::string, uint64_t> invalidated;
    uint64_t fallback[kFallbackCount] = {};
    uint64_t native_by_path[kPathCount] = {};  ///< native calls per predicted path (index = Path)
    uint64_t refused_by_path[kPathCount] = {}; ///< writer refusals per predicted path (the call ran the original)
    /// Writer refusals keyed "<path>: <reason>" (the reason is the writer's name, see Host::RefusalReason).
    std::map<std::string, uint64_t> refused_reason;
};

/// Decides, runs and verifies the calls. Not thread-safe: the glue calls it under its mutex.
class Controller {
public:
    /// Checks a full re-snapshot every kCheckInterval calls.
    static constexpr uint64_t kCheckInterval = 64;
    explicit Controller(uint32_t path_mask) : mask_(path_mask) {}
    Route Run(Host& host, const Request& req, uint64_t* node);
    /// A pool change this controller did not perform (a free, or a call that ran the original): the running
    /// expectation is no longer valid until the next verified native call. source names the cause for the
    /// report (Stats::invalidated); "unattributed" is a change nobody could name.
    void NoteUnseenChange(const char* source = "unattributed");
    bool killed() const { return killed_; }
    const std::string& kill_reason() const { return kill_reason_; }
    const Stats& stats() const { return stats_; }

private:
    void Kill(Host& host, const std::string& why);
    uint32_t mask_;
    bool killed_ = false;
    std::string kill_reason_;
    Stats stats_;
    uint64_t since_check_ = 0;
    bool expect_valid_ = false;
    Pools expect_; ///< the state after the last verified native call
};

} // namespace Native

} // namespace Model
} // namespace SotcPcPool
