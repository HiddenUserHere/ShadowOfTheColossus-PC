// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: placement by memory class, decision core (docs/re-stutters/reports3/11-placement-by-class.md).
//
// Standalone and not wired: nothing in the port calls this file yet. It answers three questions for the
// PC pool (gpu/sotc/pc_pool*.cpp) and the memory classes (mem_classes.h):
//   1. Decide: the class of a block request, from the owner object (rdi of pool_alloc_block_wrapper 0x19ae0),
//      and from the heap whose 0x12820 call is in progress (a thread-local context, optional). Two outputs:
//      the PLACEMENT class (which VA window the block should come from) and the PUBLISH class (may the
//      port untrack or drop CPU protection for it). Placement may be a guess; publication must be strict.
//   2. WindowMap: the per-class VA windows carved from the game's pool sets, and the O(1) address -> class
//      lookup the fault path can use. A window is a placement hint and a guard, never a proof of content.
//   3. VaRecycler: freed Onion/Garlic VA that the game consumes and never returns (reports3/06 section 2.3,
//      PC-MEMORY-REWRITE.md section 6), kept in a quarantine and handed back only under the safety rules
//      documented in the design report.
//
// Everything here is pure: no guest access, no host calls, no globals. Thread safety: none; the caller
// holds its own lock (the game's pool lock for the pool path, the draw thread for the classes).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace SotcPcPlace {

/// Memory classes, in the order of the design report (section 1). Unknown is the fallback: tracked as today.
enum class Class : uint8_t {
    CpuOnly = 0,    ///< CPU data the GPU never touches: may be untracked (SotcUntrack) once bind evidence agrees
    GpuStatic = 1,  ///< built once, copied to the GPU at build time (mesh and texture copies)
    GpuDynamic = 2, ///< written per frame, read by the GPU after a submit (ring candidates)
    GpuWritten = 3, ///< written by the GPU (render targets, XPR masks, stats rings); CPU reads are readbacks
    Unknown = 4,    ///< no evidence: tracked exactly as before the placement work
    kCount
};
constexpr int kClassCount = static_cast<int>(Class::kCount);
const char* ClassName(Class c);

/// SOTC_PC_PLACE: 0 off, 1 observe (classify, no change), 2 verify (place natively, compare), 3 apply.
enum class Mode : uint8_t { Off = 0, Observe = 1, Verify = 2, Apply = 3 };
const char* ModeName(Mode m);
/// Parses "0".."3" or "off", "observe", "verify", "apply". Anything else: *bad = true and Off.
Mode ParseMode(const char* text, bool* bad);

/// Page granule of the pool (every run is a multiple of it).
constexpr uint64_t kPlacePage = 0x10000;
/// Alignment of the S1/S2 search (pool_alloc_region rounds the alignment up to 1 MiB).
constexpr uint64_t kPlaceMiB = 0x100000;
/// Pool VA bounds (runtime_memory.c USER_MIN / USER_MAX). The PS4 code packs pointers into 40-bit fields, so
/// every window must end below kVaFieldLimit as well (the 31-bit limit applies to the image, not to pool VA).
constexpr uint64_t kVaFloor = UINT64_C(0x1000000000);
constexpr uint64_t kVaCeiling = UINT64_C(0xfc00000000);
constexpr uint64_t kVaFieldLimit = UINT64_C(1) << 40;

// ---- decision ------------------------------------------------------------------------------------------

/// The owner object of a block request (family object, rdi): its guest address, name (+0x10 char*) and kind
/// (+0x18: 1 CPU, 2 GPU, 0 for kind-0 objects). addr 0 means the owner is unreadable.
struct OwnerKey {
    uint64_t addr = 0;
    const char* name = "";
    int32_t kind = 0;
};

/// One request: the owner and, when the request runs inside a 0x12820 call, the memtype heap slot of that call
/// (-1 when there is none). The heap slot is the more specific source; see Decide.
struct Request {
    OwnerKey owner;
    int32_t heap_slot = -1;
};

/// A class rule. Addr matches one owner object exactly; NameKind matches a name and kind (the names repeat across
/// kinds, so both are needed); Heap matches a memtype slot.
struct Rule {
    enum class Match : uint8_t { Addr, NameKind, Heap };
    Match match = Match::NameKind;
    uint64_t addr = 0;
    const char* name = "";
    int32_t kind = 0;
    int32_t heap_slot = -1;
    Class cls = Class::Unknown;
    const char* label = ""; ///< for the logs (for example "RenderSingleFrameGPU")
};

class ClassTable {
public:
    /// Adds a rule. Returns false with *why set when the rule is malformed (Addr with addr 0, NameKind with no name,
    /// Heap with a negative slot).
    bool Add(const Rule& rule, std::string* why);
    size_t size() const { return rules_.size(); }
    void Clear() { rules_.clear(); }
    const std::vector<Rule>& rules() const { return rules_; }

private:
    std::vector<Rule> rules_;
};

/// The result of Decide.
struct Decision {
    Class place = Class::Unknown;   ///< where the block should come from (window class)
    Class publish = Class::Unknown; ///< may be untracked / unprotected, before the window and bind checks
    bool owner_matched = false;
    bool heap_matched = false;
    Class owner_class = Class::Unknown; ///< valid when owner_matched
    Class heap_class = Class::Unknown;  ///< valid when heap_matched
    const char* reason = "";            ///< one of the kReason* strings
};

// Reasons. Static strings, so the report can count them without allocation.
constexpr const char* kReasonNoRule = "no rule";
constexpr const char* kReasonOwnerOnly = "owner rule";
constexpr const char* kReasonHeapOnly = "heap rule";
constexpr const char* kReasonAgree = "owner and heap agree";
constexpr const char* kReasonDisagree = "owner and heap disagree";

/// Decides the class of one request. Rules:
///   - the heap rule (when the request has a heap slot and a rule matches it) is the placement, because the heap
///     is more specific than the shared owner (a CPU arena hosts several heaps, reports3/05);
///   - else the owner rule (Addr before NameKind, table order within a kind); else Unknown;
///   - publish equals place only when every source that matched agrees with it. Any disagreement, or no source at
///     all, publishes Unknown. In particular a GPU heap inside a CPUMemory owner (RenderSingleFrameCPU, slot 17,
///     which the GPU writes) places GpuWritten and publishes Unknown.
Decision Decide(const Request& req, const ClassTable& table);

// ---- VA windows ----------------------------------------------------------------------------------------

struct VaWindow {
    uint64_t base = 0;
    uint64_t size = 0;
    Class cls = Class::Unknown;
};

/// The windows of the pool VA, sorted by base, non-overlapping. Addresses outside every window are Unknown.
class WindowMap {
public:
    /// Validates and installs the windows: each base and size a multiple of 1 MiB, size non-zero, inside
    /// [kVaFloor, kVaCeiling), no two windows overlapping, cls not Unknown (an unwindowed address is Unknown anyway).
    /// Returns false with *why set and the old map kept.
    bool Set(std::vector<VaWindow> windows, std::string* why);
    /// The class of the window holding va, or Unknown.
    Class ClassOf(uint64_t va) const;
    /// The window holding va. False when none.
    bool Find(uint64_t va, VaWindow* out) const;
    const std::vector<VaWindow>& windows() const { return windows_; }
    size_t size() const { return windows_.size(); }

private:
    std::vector<VaWindow> windows_;
};

/// Carves [base, base + size) into consecutive windows of the given byte sizes, in order. Each part is a non-zero
/// multiple of 1 MiB; the parts must fit in size (the rest stays unwindowed, Unknown). Returns the windows; on error
/// returns an empty vector and sets *why.
std::vector<VaWindow> CarveWindows(uint64_t base, uint64_t size,
                                   const std::vector<std::pair<Class, uint64_t>>& parts, std::string* why);

/// The class a block gets once its owner decision, its window and the GPU bind evidence are known. Returns
/// decision.publish when:
///   - the window of the block's VA has the same class (a block placed outside its class's window is not trusted);
///   - and, for CpuOnly, gpu_bound is false (NoteGpuBind saw a bind overlapping the block, sticky).
/// Otherwise Unknown. Pure; the fault path and the publish list call it with the same inputs.
Class Effective(const Decision& decision, Class window_class, bool gpu_bound);

/// Which heap classes have carved a block. A block can hold several heaps' data (reports3/05: the 9 block owners hold
/// every named heap), so the block class is sticky: the first carved heap class is kept, and a second, different class
/// makes the block Unknown for good (the same rule as the granule "shared" exclusion in mem_classes.cpp).
class BlockLedger {
public:
    /// Records that a heap of class cls carved from the block at base (any heap, any time before the block is freed).
    void NoteCarve(uint64_t base, Class cls);
    /// The block's class: Unknown for an unseen block or a sticky conflict.
    Class Get(uint64_t base) const;
    /// True once a second, different class carved from the block.
    bool Conflicted(uint64_t base) const;
    /// The block was freed: forget it. Its VA may be recycled (VaRecycler), and a new block starts clean.
    void Forget(uint64_t base);
    size_t size() const { return blocks_.size(); }

private:
    struct Entry {
        Class cls = Class::Unknown;
        bool conflict = false;
    };
    std::map<uint64_t, Entry> blocks_;
};

// ---- VA recycling -------------------------------------------------------------------------------------

/// Recycling of freed pool VA. The game unmaps a freed Onion/Garlic block and consumes its VA, so the VA never comes
/// back (about 700 frees per window, PC-MEMORY-REWRITE.md section 6). A freed run is retired here instead and handed
/// back to a fresh block of the same class only when Take says it is safe:
///   1. Retire needs the VA unmapped (the caller's PhysMap says so after the batch flush) and a window class; double
///      frees and overlaps with a retired run are refused.
///   2. Quarantine: the run is not reused before retire_frame + quarantine_frames (a stale pointer to the freed block
///      still reads zero or unmapped memory for a while, and a use-after-free stays visible in verify mode).
///   3. GPU fence: the GPU must have finished every submit up to the retire frame (gpu_done_frame >= retire_frame).
///      A bind of the old block may still be in a descriptor or a queued draw.
///   4. Cache invalidation: MarkInvalidated must have been called for the run after the buffer and texture caches
///      dropped every entry overlapping it. Until then the run is not reusable.
///   5. Class purity: a run is only taken by a block of the same class, from the same window.
/// Modes: Off never retires or takes; Quarantine retires and counts the reuses it would have made but never takes
/// (verify); Recycle takes.
class VaRecycler {
public:
    enum class Mode : uint8_t { Off = 0, Quarantine = 1, Recycle = 2 };
    struct Stats {
        uint64_t retired = 0;          ///< runs retired
        uint64_t retired_bytes = 0;
        uint64_t taken = 0;            ///< runs handed out (Recycle mode)
        uint64_t taken_bytes = 0;
        uint64_t would_reuse = 0;      ///< Quarantine mode: a take that Recycle would have made
        uint64_t refused_retire = 0;   ///< bad size or alignment, unmapped not confirmed, window Unknown, overlap
        uint64_t refused_quarantine = 0;
        uint64_t refused_gpu = 0;
        uint64_t refused_invalid = 0;
        uint64_t merged = 0;           ///< adjacent retired runs coalesced
    };

    VaRecycler(Mode mode, uint64_t quarantine_frames) : mode_(mode), quarantine_(quarantine_frames) {}

    /// Retires [va, va + size) of class cls, freed at frame. unmapped_confirmed: the caller's shadow map shows the whole
    /// range unmapped after the flush. Returns false with *why set when refused.
    bool Retire(uint64_t va, uint64_t size, Class cls, uint64_t frame, bool unmapped_confirmed, std::string* why);
    /// The caches dropped every entry overlapping [va, va + size). Only retired runs fully inside the range are marked.
    void MarkInvalidated(uint64_t va, uint64_t size);
    /// A fresh block of class cls, size bytes, aligned to align (0 means 64 KiB). Returns true and the VA when a retired
    /// run satisfies every rule (Recycle mode only; the run is split, the remainder stays retired).
    bool Take(Class cls, uint64_t size, uint64_t align, uint64_t now_frame, uint64_t gpu_done_frame, uint64_t* va_out,
              std::string* why);
    Mode mode() const { return mode_; }
    uint64_t quarantine_frames() const { return quarantine_; }
    const Stats& stats() const { return stats_; }
    size_t runs() const { return runs_.size(); }
    uint64_t retired_bytes_now() const;

private:
    struct Run {
        uint64_t size = 0;
        Class cls = Class::Unknown;
        uint64_t retire_frame = 0;
        bool invalidated = false;
    };
    bool Ready(const Run& r, uint64_t now_frame, uint64_t gpu_done_frame, const char** why) const;
    Mode mode_;
    uint64_t quarantine_;
    std::map<uint64_t, Run> runs_; ///< VA start -> run, runs never overlap
    Stats stats_;
};

} // namespace SotcPcPlace
