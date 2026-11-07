// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC memory model, wave B memtype probe (docs/re-stutters/PC-MEMORY-REWRITE.md section 5;
// reports3/04-alloc-scopes.md). Hooks are declared in scripts/hooks.d/memtype.py, switch
// SOTC_HOOK_MEMTYPE_PROBE, one group (all or nothing):
//   MEMTYPE_ALLOC       wrap at the generic allocator 0x12820(size, memtype, align, flags).
//                       Pre: rdi = size, esi = memtype slot (clamped to 0..63), [rsp] = the
//                       caller's return address. Post (only with SOTC_MEMTYPE_CLASSES=1): rax =
//                       the returned pointer; its 64 KiB granules are registered with
//                       SotcMemClasses as owner blocks of the slot (see "granule claims" below).
//   MEMTYPE_FREE        entry at the generic free 0x12af0(ptr, esi). Pre: rdi = the block; SOTC_ALLOC_SITES ends it.
//   MEMTYPE_FREE_SMALL / MEMTYPE_FREE_BPE  entries at the heap frees 0x17e90 / 0x493d0 (rdi = heap, rsi = block).
//   MEMTYPE_REALLOC     wrap at the realloc 0x12c50(ptr, size, align, flags); post rax = the block (moved or not).
//                       These four only feed SOTC_ALLOC_SITES (group alloc_sites_free).
//   RENDER_CTX_REQUEST  entry at PerFrameRenderContexts::RequestNewRenderContext (0x90470).
//                       Pre: rsi = DCB size, 0 meaning the 0x500000 default (rdi = list, rdx = CCB
//                       size, rcx = this). One call per render context chunk.
//   MEMTYPE_FLIP_SITE   entry at sub_8c860, the RenderBackend_Frame flip pair (RenderSingleFrameGPU
//                       and RenderSingleFrameCPU rings). Its derived rip_target fields give the two
//                       globals. The ring flip 0x136f0 itself cannot be detoured (indirect branch),
//                       so the flips of the other heaps are not counted.
// A frame is one call of sub_8c860, made once per frame after every writer. Per-frame byte totals
// and chunk counts are closed there.
//
// Observe only: every handler returns HookAction::Continue and writes nothing in the guest. The
// handlers are lock-free (atomics and fixed arrays), allocate nothing and log nothing per call.
// The window reset in ReportMemtype is best effort across threads: a call racing the reset can be
// counted in the next window. Guest reads are limited to the caller's return address on the guest
// stack (committed, never GPU-tracked) and to the image's heap-id globals.

#include "pc_ring.h"
// sotc: SOTC_PC_RING_HEAPSTAT (pc_ring.cpp). Declared here; pc_ring.h should carry these two declarations.
namespace SotcPcRing {
bool HeapStatOn();
void NoteHeapAlloc(uint32_t slot, uint64_t addr, uint64_t size);
} // namespace SotcPcRing
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "hooks.h"
#include "pc_pool_place_glue.h" // sotc: SOTC_PC_PLACE heap carves
#include "mem_classes.h" // SotcMemClasses::OnGranule / OnGranuleShared (granule claims)
#include "alloc_sites.h" // sotc: SOTC_ALLOC_SITES
#include "gpu_region.h"  // sotc: SOTC_GPU_PLACE (the port-owned region)
#include "gpu_sites.h"   // sotc: SOTC_GPU_PLACE (the GPU-written allocation sites)
#include "native/n1verify.h" // sotc: SOTC_VERIFY=n1 allocator class feed
#include "probes.h"
#include "native/native.h"               // sotc: SOTC_NATIVE switch (SotcNative::On)
#include "native/native_draw.h"          // sotc: SOTC_NATIVE_OWNER_DISCARD (SotcNative::DiscardOwner)

namespace SotcNativeHooks { // sotc: gpu/sotc/native/native_hooks.cpp
void NoteFrameBlock(uint64_t addr, uint64_t size);
} // namespace SotcNativeHooks
#include "video_core/sotc_trace.h" // image bounds (SotcTrace::ImageBase/ImageSize)

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr size_t kSlots = 64;              // memtype slots tracked; larger ids clamp to kSlots - 1
constexpr size_t kCallers = 64;            // caller addresses per slot (open addressing, power of two)
constexpr size_t kTopCallers = 8;          // callers listed per slot in the report
constexpr size_t kSizeBins = 8;            // distinct render context DCB sizes kept
constexpr uint64_t kDefaultDcb = 0x500000; // RequestNewRenderContext default when the size argument is 0
constexpr uint64_t kOutside = ~uint64_t{0}; // caller key for return addresses outside the eboot image
constexpr size_t kFrameGlobal = 3;         // index of RenderSingleFrameGPU in kGlobals: the frame boundary
static_assert((kCallers & (kCallers - 1)) == 0, "kCallers must be a power of two");

// Heap-id globals (memtype slot ids, u32). The address is derived from the MEMTYPE_FLIP_SITE
// instructions when that hook resolved; otherwise the fingerprint-keyed fallback (EU 1.01 and
// US 1.00 share this code and data) is used.
struct GlobalDef {
    const char* name;
    uint64_t fallback; // image offset (eboot VA)
    const char* derived; // field of MEMTYPE_FLIP_SITE, or nullptr
};
constexpr std::array<GlobalDef, 11> kGlobals{{
    {"GameToRenderThread", 0x2ed1f48, nullptr},
    {"GameToGPU", 0x2ed1f4c, nullptr},
    {"CPU_RenderToGPUToGame", 0x2ed1f50, nullptr},
    {"RenderSingleFrameGPU", 0x2ed1f54, "single_frame_gpu"},
    {"RenderSingleFrameCPU", 0x2ed1f58, "single_frame_cpu"},
    {"MaterialParamHeap", 0x2ed1f5c, nullptr},
    {"VirtualMemoryGPU", 0x2ed1f60, nullptr},
    {"CPUMemory", 0x2dd9f80, nullptr},
    {"GPUMemory", 0x2dd9f84, nullptr},
    {"GameSingleFrame", 0x2dd9f88, nullptr},
    {"GameDoubleFrame", 0x2dd9f8c, nullptr},
}};
constexpr size_t kGlobalCount = kGlobals.size();
constexpr size_t kOtherGlobal = kGlobalCount; // flips of any other address

std::array<uint64_t, kGlobalCount> g_global_off{};
std::once_flag g_global_once;

void ResolveGlobals() {
    for (size_t i = 0; i < kGlobalCount; ++i) {
        uint64_t off = kGlobals[i].fallback;
        uint64_t derived = 0;
        if (kGlobals[i].derived && SotcHooks::Derived("MEMTYPE_FLIP_SITE", kGlobals[i].derived, &derived)) {
            off = derived;
        }
        g_global_off[i] = off;
    }
}

// Resolved once, on the first flip or report (after InstallHooks, before any guest code runs).
void EnsureGlobals() {
    std::call_once(g_global_once, &ResolveGlobals);
}

// Image offset of a host address inside the mapped eboot. False when outside the image.
bool ImageOffset(uint64_t address, uint64_t* off) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (base == 0 || size == 0 || address < base || address - base >= size) {
        return false;
    }
    *off = address - base;
    return true;
}

// Reads a u32 at an image offset (the heap-id globals live in the image data).
bool ReadImageU32(uint64_t off, uint32_t* out) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (base == 0 || size < 4 || off > size - 4) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(base + off)), sizeof *out);
    return true;
}

// Lock-free maximum.
void AtomicMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

// One caller of a slot: its return address (image offset, or kOutside) with calls and bytes.
struct CallerBin {
    std::atomic<uint64_t> key{0}; // 0 = empty
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> bytes{0};
};

// One memtype slot. calls/bytes are the report window; frame_bytes is the running total since the
// last frame boundary (not reset by the report), closed into frame_sum/frame_max at each flip.
struct SlotStats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> frame_bytes{0};
    std::atomic<uint64_t> frame_sum{0};
    std::atomic<uint64_t> frame_max{0};
    std::atomic<uint64_t> caller_overflow{0};
    std::array<CallerBin, kCallers> callers{};
};
std::array<SlotStats, kSlots> g_slots{};
std::atomic<uint64_t> g_slot_clamped{0}; // allocations whose memtype slot was >= kSlots

// Adds one allocation to the caller table of a slot. Keys persist across windows (only the counts
// reset), so a caller keeps its bin once it has one.
void AddCaller(SlotStats& s, uint64_t key, uint64_t bytes) {
    const size_t start = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 58) & (kCallers - 1);
    for (size_t probe = 0; probe < kCallers; ++probe) {
        CallerBin& c = s.callers[(start + probe) & (kCallers - 1)];
        uint64_t k = c.key.load(std::memory_order_acquire);
        if (k == 0) {
            uint64_t expected = 0;
            if (c.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                k = key;
            } else {
                k = expected; // another thread claimed the bin with another key
            }
        }
        if (k == key) {
            c.calls.fetch_add(1, std::memory_order_relaxed);
            c.bytes.fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
    }
    s.caller_overflow.fetch_add(1, std::memory_order_relaxed);
}

// Render context chunks: per window, the DCB sizes seen (a few distinct values) and the per-frame
// chunk count. g_ctx_frame counts the chunks since the last frame boundary.
struct SizeBin {
    std::atomic<uint64_t> size{0}; // 0 = empty
    std::atomic<uint64_t> count{0};
};
std::array<SizeBin, kSizeBins> g_ctx_sizes{};
std::atomic<uint64_t> g_ctx_size_overflow{0};
std::atomic<uint64_t> g_ctx_requests{0};
std::atomic<uint64_t> g_ctx_frame{0};
std::atomic<uint64_t> g_ctx_sum{0}; // window: sum over frames of the chunk count
std::atomic<uint64_t> g_ctx_max{0}; // window: largest per-frame chunk count

void AddCtxSize(uint64_t size) {
    for (SizeBin& b : g_ctx_sizes) {
        uint64_t k = b.size.load(std::memory_order_acquire);
        if (k == 0) {
            uint64_t expected = 0;
            if (b.size.compare_exchange_strong(expected, size, std::memory_order_acq_rel)) {
                k = size;
            } else {
                k = expected;
            }
        }
        if (k == size) {
            b.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_ctx_size_overflow.fetch_add(1, std::memory_order_relaxed);
}

// Window counters of the ring flips: one per known heap-id global, plus the other addresses.
std::array<std::atomic<uint64_t>, kGlobalCount + 1> g_flips{};

// A frame boundary: closes the per-frame byte totals and chunk count of the frame that just ended.
void FinishFrame() {
    const uint64_t chunks = g_ctx_frame.exchange(0, std::memory_order_relaxed);
    g_ctx_sum.fetch_add(chunks, std::memory_order_relaxed);
    AtomicMax(g_ctx_max, chunks);
    for (SlotStats& s : g_slots) {
        const uint64_t f = s.frame_bytes.exchange(0, std::memory_order_relaxed);
        if (f == 0) {
            continue;
        }
        s.frame_sum.fetch_add(f, std::memory_order_relaxed);
        AtomicMax(s.frame_max, f);
    }
}

// ---------------------------------------------------------------------------
// Granule claims (SOTC_MEMTYPE_CLASSES=1, with SOTC_HOOK_MEMTYPE_PROBE=1)
//
// Each allocation of a slot is coalesced into 64 KiB granules. The first slot that touches a granule
// owns it and registers it with SotcMemClasses::OnGranule (its own table, pool "memtype:<heap-id
// globals naming the slot>"). Later touches by another slot are counted as shared, and the first one
// also tells SotcMemClasses::OnGranuleShared, which keeps the granule out of CPU_ONLY. Granules are
// never freed: the rings reuse them.
//
// The pre phase stores the size and slot of each call in a small thread-local stack; the post phase
// pops it and claims the granules of the returned pointer. Only the claim (first touch of a granule)
// takes the SotcMemClasses lock and allocates; a call inside an already claimed granule costs a few
// atomic loads.
// ---------------------------------------------------------------------------

constexpr uint64_t kGranuleShift = 16;                   // 64 KiB granules
constexpr uint64_t kGranuleSize = uint64_t{1} << kGranuleShift;
// sotc: 131072 granules (8 GiB) since 1 Nov: 32768 filled at boot (CPUMemory 14398 + GPUMemory 12760 + slot20 5160)
// and left ~3.6M touches per session unattributed. Must match kMaxGranuleEntries (mem_classes.h).
constexpr size_t kMaxGranules = 131072;                  // claimed granules, all slots
constexpr size_t kGranuleTableBits = 18;
constexpr size_t kGranuleTable = size_t{1} << kGranuleTableBits; // open addressing, twice the claim cap
constexpr size_t kPendingDepth = 8;                      // nested MEMTYPE_ALLOC pre phases kept
static_assert((kGranuleTable & (kGranuleTable - 1)) == 0, "kGranuleTable must be a power of two");
static_assert(kMaxGranules * 2 <= kGranuleTable, "the table must stay at most half full");
static_assert(kMaxGranules == SotcMemClasses::kMaxGranuleEntries, "the granule caps must match");

// One claimed granule. word = (granule index + 1) << 8 | owner slot, 0 = empty; set by one CAS, so
// the owner is never observed half-written. foreign: bit s set when slot s touched the granule
// without owning it (counted once per slot and granule).
struct GranuleEntry {
    std::atomic<uint64_t> word{0};
    std::atomic<uint64_t> foreign{0};
    std::atomic<uint64_t> label{0}; // hash of the slot label the pool was named with (0 = not registered yet)
    // sotc: SOTC_FAULT_OWNERS. The last allocation that touched the granule: slot << 56 | caller (image offset, or
    // the low 48 bits of kOutside), and its size.
    std::atomic<uint64_t> last_owner{0};
    std::atomic<uint64_t> last_size{0};
};
std::array<GranuleEntry, kGranuleTable> g_granules{};
std::atomic<uint64_t> g_granule_claims{0};              // claimed granules (table fill, <= kMaxGranules)
std::array<std::atomic<uint64_t>, kSlots> g_slot_granules{};
std::atomic<uint64_t> g_granule_shared{0};              // granules touched by more than one slot
std::atomic<uint64_t> g_granule_overflow{0};            // granule touches that could not be claimed
bool g_classes_on = false;                              // SOTC_MEMTYPE_CLASSES, read at registration

// Size and slot of a MEMTYPE_ALLOC pre phase, read again by its post phase (same guest thread). prev_heap is the
// placement glue's heap slot before this call, restored by the post phase (sotc: SOTC_PC_PLACE heap context).
struct PendingAlloc {
    uint64_t size = 0;
    uint32_t slot = 0;
    int32_t prev_heap = -1;
    uint64_t caller = 0; // sotc: SOTC_FAULT_OWNERS: the caller key (image offset or kOutside)
    uint64_t caller2 = 0; // sotc: SOTC_ALLOC_SITES: the two callers above it (frame-pointer chain)
    uint64_t caller3 = 0;
    uint32_t trace = ~0u; // sotc: SOTC_ALLOC_TRACE record
};
thread_local std::array<PendingAlloc, kPendingDepth> t_pending{};
thread_local uint32_t t_pending_depth = 0;
// The memtype whose blocks are RenderSingleFrameCPU's (the per-frame CPU arena): NoteFrameBlock announces them.
constexpr int64_t kRenderSingleFrameCpuSlot = 17;
// sotc: the render-target pool heap (slot 20, alloc_sites.tsv): its blocks are carved into surfaces by the engine.
constexpr uint32_t kRenderTargetSlot = 20;

// sotc: SOTC_GPU_PLACE=1. Allocations of the GPU-written sites (gpu_sites.h) are served from the port-owned GPU region
// (gpu_region.h) instead of the engine heap, so the engine's heaps never hold GPU-written bytes: no stale GPU marks on
// reused CPU memory and no CPU object sharing a page with GPU data. The engine's heap never sees such a block, so every
// path that takes one back is answered here: the generic free 0x12af0, the heap frees 0x17e90 / 0x493d0, the realloc
// 0x12c50 and the pointer classifier 0x19a50 (docs/re-stutters/HEAP-MAP.md, "Port constraints"). A site is placed only
// when its heap's free (vtbl+0xa8) is one of those intercepted (small-object, BPE) or the render-heap no-op (a ring:
// its blocks expire by frame in the region). A freed block waits in the region's quarantine while the buffer cache
// forgets its GPU-written state at the command processor's point, so a new owner never sees old contents.
namespace place {
constexpr uint64_t kHeapRegistry = 0x2dfaab0; // image offset: heap object per memtype slot
constexpr uint64_t kFreeSmall = 0x17e90;      // vtbl+0xa8 targets the region answers
constexpr uint64_t kFreeBpe = 0x493d0;
constexpr uint64_t kFreeRing = 0x1d9b0;       // render heaps: free is a no-op (ring)
constexpr uint64_t kFreeFamily = 0x47f30;     // GPUMemory / CPUMemory family heaps: free forwarder
// The family free 0x47f30 cannot be hooked (it ends in an indirect jmp). It asks the classifier 0x19a50 for the
// block's owner and jumps to owner->vtbl[+0xa8]. For a region block, the classifier called from there (return
// address 0x47f45) answers a port-owned stand-in heap whose vtbl+0xa8 is the small-object free 0x17e90, which is
// hooked (MEMTYPE_FREE_SMALL) and takes the block back. Other classifier callers get the family heap itself.
constexpr uint64_t kFamilyClassifyRet = 0x47f45;
struct alignas(16) StandInHeap {
    uint64_t vptr;
    uint64_t pad[31];
};
alignas(16) uint64_t g_stand_in_vtbl[64];
StandInHeap g_stand_in{};
uint64_t StandInHeapAddress() {
    static const uint64_t addr = [] {
        g_stand_in_vtbl[0xa8 / 8] = SotcTrace::ImageBase() + kFreeSmall;
        g_stand_in.vptr = reinterpret_cast<uint64_t>(&g_stand_in_vtbl[0]);
        return reinterpret_cast<uint64_t>(&g_stand_in);
    }();
    return addr;
}
std::atomic<int> g_state{0};                  // 0 not decided, 1 on, -1 off
std::mutex g_init_mutex;
std::array<std::atomic<int8_t>, 64> g_slot_kind{}; // 0 unknown, 1 normal heap, 2 ring heap, -1 not placeable
std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_placed{0}, g_placed_bytes{0}, g_fallback{0}, g_refused_slot{0}, g_freed{0}, g_classified{0},
    g_reallocs{0};

void Quarantined(void*, uint64_t addr, uint64_t size) {
    SotcNative::DiscardRegion(addr, size);
}

bool On() {
    return g_state.load(std::memory_order_acquire) > 0;
}

// Decided at the first allocation: every intercept must be installed, and the region reserved.
bool Ready() {
    const int s = g_state.load(std::memory_order_acquire);
    if (s != 0) {
        return s > 0;
    }
    std::scoped_lock lock{g_init_mutex};
    if (g_state.load(std::memory_order_relaxed) != 0) {
        return g_state.load(std::memory_order_relaxed) > 0;
    }
    const char* missing = nullptr;
    for (const char* name : {"MEMTYPE_FREE", "MEMTYPE_FREE_SMALL", "MEMTYPE_FREE_BPE", "MEMTYPE_CLASSIFY",
                             "MEMTYPE_REALLOC", "MEMTYPE_FLIP_SITE"}) {
        if (!SotcHooks::IsInstalled(name)) {
            missing = name;
            break;
        }
    }
    bool ok = missing == nullptr;
    if (ok) {
        SotcGpuSites::Load();
        ok = SotcGpuRegion::Init(0);
        if (ok) {
            SotcGpuRegion::SetQuarantineHook(&Quarantined, nullptr);
        }
    }
    std::fprintf(stderr, "SOTC GPU PLACE: %s%s%s\n", ok ? "on" : "off",
                 missing ? " (hook not installed: " : (ok ? "" : " (region reservation failed)"),
                 missing ? missing : "");
    if (missing) {
        std::fprintf(stderr, ")\n");
    }
    g_state.store(ok ? 1 : -1, std::memory_order_release);
    return ok;
}

uint64_t HeapOf(uint32_t slot) {
    const uint64_t base = SotcTrace::ImageBase();
    if (base == 0 || slot >= 64) {
        return 0;
    }
    uint64_t heap = 0;
    std::memcpy(&heap, reinterpret_cast<const void*>(static_cast<uintptr_t>(base + kHeapRegistry + slot * 8)),
                sizeof heap);
    return heap;
}

// 1 = a heap whose blocks are freed through an intercepted free, 2 = a ring heap, -1 = not placeable, 0 = no heap yet.
int SlotKind(uint32_t slot) {
    if (slot >= g_slot_kind.size()) {
        return -1;
    }
    const int8_t known = g_slot_kind[slot].load(std::memory_order_relaxed);
    if (known != 0) {
        return known;
    }
    const uint64_t heap = HeapOf(slot);
    if (heap == 0) {
        return 0; // the registry fills lazily: ask again later
    }
    uint64_t vptr = 0;
    uint64_t free_fn = 0;
    std::memcpy(&vptr, reinterpret_cast<const void*>(static_cast<uintptr_t>(heap)), sizeof vptr);
    if (vptr != 0) {
        std::memcpy(&free_fn, reinterpret_cast<const void*>(static_cast<uintptr_t>(vptr + 0xa8)), sizeof free_fn);
    }
    uint64_t off = 0;
    int8_t kind = -1;
    if (ImageOffset(free_fn, &off)) {
        if (off == kFreeSmall || off == kFreeBpe || off == kFreeFamily) {
            kind = 1;
        } else if (off == kFreeRing) {
            kind = 2;
        }
    }
    g_slot_kind[slot].store(kind, std::memory_order_relaxed);
    if (kind < 0) {
        std::fprintf(stderr, "SOTC GPU PLACE: slot %u not placeable (heap free at %#llx)\n", slot,
                     (unsigned long long)off);
    }
    return kind;
}

// Pre phase of 0x12820 for a listed site: the block from the region, or 0 (the engine allocates as usual).
uint64_t TryPlace(uint64_t c1, uint64_t c2, uint64_t c3, uint32_t slot, uint64_t size, uint64_t align) {
    uint32_t flags = 0;
    if (size == 0 || !SotcGpuSites::Lookup(c1, c2, c3, slot, &flags)) {
        return 0;
    }
    const int kind = SlotKind(slot);
    if (kind <= 0) {
        g_refused_slot.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (align > (uint64_t{1} << 20)) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const uint64_t addr = SotcGpuRegion::Alloc(size, align < 16 ? 16 : align, slot,
                                               kind == 2 ? SotcGpuRegion::kKindRing : SotcGpuRegion::kKindNormal);
    if (addr == 0) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    g_placed.fetch_add(1, std::memory_order_relaxed);
    g_placed_bytes.fetch_add(size, std::memory_order_relaxed);
    return addr;
}

// A region pointer reached a free: the region takes it back (quarantine). True when the pointer was the region's.
bool TakeFree(uint64_t ptr) {
    if (!On() || !SotcGpuRegion::Contains(ptr)) {
        return false;
    }
    SotcGpuRegion::Free(ptr); // a block not live (a double free) is only counted there
    g_freed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Report(std::string& out) {
    if (!On()) {
        return;
    }
    std::string region;
    SotcGpuRegion::Report(region);
    std::string sites;
    SotcGpuSites::Report(sites);
    SotcGpuSites::MaybeSave();
    char line[512];
    std::snprintf(line, sizeof line,
                  "SOTC GPU PLACE: placed %llu (%.1f MiB), frees %llu, classified %llu, reallocs %llu, fallback %llu, "
                  "slot refused %llu; region:%s; sites:%s\n",
                  (unsigned long long)g_placed.exchange(0), g_placed_bytes.exchange(0) / 1048576.0,
                  (unsigned long long)g_freed.exchange(0), (unsigned long long)g_classified.exchange(0),
                  (unsigned long long)g_reallocs.exchange(0), (unsigned long long)g_fallback.exchange(0),
                  (unsigned long long)g_refused_slot.exchange(0), region.c_str(), sites.c_str());
    out += line;
}
} // namespace place
bool g_post_on = false; // the post phase is registered: SOTC_MEMTYPE_CLASSES, the placement glue or SOTC_NATIVE
bool g_post_granules = false; // the post phase also claims granules: SOTC_MEMTYPE_CLASSES or the placement glue

HookAction AllocPre(GuestRegs& regs, void*) {
    const uint64_t size = regs.rdi;
    const uint64_t raw_slot = regs.rsi & 0xffffffffull;
    size_t idx = static_cast<size_t>(raw_slot);
    if (raw_slot >= kSlots) {
        idx = kSlots - 1;
        g_slot_clamped.fetch_add(1, std::memory_order_relaxed);
    }
    // At a wrap's pre phase rsp points at the return address; the guest stack is host memory and
    // committed, so it is read directly (as probe_dt.cpp reads its stack window).
    uint64_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof ret);
    uint64_t key = kOutside;
    uint64_t off = 0;
    if (ImageOffset(ret, &off)) {
        key = off;
    }
    // sotc: SOTC_ALLOC_SITES: two more return addresses through the frame-pointer chain. At the wrap's pre phase rbp
    // is the caller's frame pointer, so [rbp+8] returns into the caller's caller and [[rbp]+8] one level higher. A
    // frame pointer outside the stack window above rsp ends the chain (a frameless caller).
    uint64_t key2 = kOutside;
    uint64_t key3 = kOutside;
    if (SotcAllocSites::On() || SotcGpuSites::On()) {
        const uint64_t sp = regs.rsp;
        uint64_t fp = regs.rbp;
        const auto in_stack = [](uint64_t p, uint64_t lo) { return p >= lo && p < lo + 0x200000 && (p & 7) == 0; };
        if (in_stack(fp, sp)) {
            uint64_t r2 = 0;
            std::memcpy(&r2, reinterpret_cast<const void*>(static_cast<uintptr_t>(fp + 8)), sizeof r2);
            uint64_t o = 0;
            if (ImageOffset(r2, &o)) {
                key2 = o;
            }
            uint64_t fp2 = 0;
            std::memcpy(&fp2, reinterpret_cast<const void*>(static_cast<uintptr_t>(fp)), sizeof fp2);
            if (in_stack(fp2, fp + 8)) {
                uint64_t r3 = 0;
                std::memcpy(&r3, reinterpret_cast<const void*>(static_cast<uintptr_t>(fp2 + 8)), sizeof r3);
                if (ImageOffset(r3, &o)) {
                    key3 = o;
                }
            }
        }
    }
    // sotc: SOTC_GPU_PLACE: a listed site's block comes from the GPU region; the engine's allocator does not run (no
    // pending entry, no heap context: the post phase is skipped with it).
    if (SotcGpuSites::On() && raw_slot < kSlots && place::Ready()) {
        const uint64_t placed = place::TryPlace(key, key2, key3, static_cast<uint32_t>(raw_slot), size,
                                                regs.rdx & 0xffffffffull);
        if (placed != 0) {
            regs.rax = placed;
            // The post phase is skipped: a RenderSingleFrameCPU block is announced PerFrame here instead (as in
            // AllocPost), so the CPU's per-frame writes still reach the GPU.
            if (SotcNative::On() && static_cast<int64_t>(raw_slot) == kRenderSingleFrameCpuSlot) {
                SotcNativeHooks::NoteFrameBlock(placed, size);
            }
            if (SotcAllocSites::On()) {
                SotcAllocSites::NoteAlloc(placed, size, static_cast<uint32_t>(raw_slot), key, key2, key3);
            }
            return HookAction::Return;
        }
    }
    uint32_t trace = ~0u;
    if (SotcAllocSites::On()) { // sotc: SOTC_ALLOC_TRACE: the registers, stack and objects of a traced site
        trace = SotcAllocSites::Capture(key, key2, key3, size, reinterpret_cast<const uint64_t*>(&regs));
    }
    if (g_post_on) {
        // sotc: SOTC_PC_PLACE: this call is the heap context of every pool block its refill hands out.
        const int32_t prev = SotcPcPlaceGlue::PushHeapSlot(static_cast<int32_t>(idx));
        t_pending[t_pending_depth % kPendingDepth] =
            PendingAlloc{size, static_cast<uint32_t>(idx), prev, key, key2, key3, trace};
        ++t_pending_depth;
    }
    // sotc: with SOTC_GPU_PLACE the hook is on for the placement; the per-call statistics (SOTC MEMTYPE lines) then
    // run only with SOTC_MEMTYPE_STATS=1.
    static const bool stats = [] {
        const char* v = std::getenv("SOTC_MEMTYPE_STATS");
        return !SotcGpuSites::On() || (v && v[0] == '1');
    }();
    if (stats) {
        SlotStats& s = g_slots[idx];
        s.calls.fetch_add(1, std::memory_order_relaxed);
        s.bytes.fetch_add(size, std::memory_order_relaxed);
        s.frame_bytes.fetch_add(size, std::memory_order_relaxed);
        AddCaller(s, key, size);
    }
    return HookAction::Continue;
}

HookAction RenderCtxPre(GuestRegs& regs, void*) {
    const uint64_t dcb = regs.rsi != 0 ? regs.rsi : kDefaultDcb;
    g_ctx_requests.fetch_add(1, std::memory_order_relaxed);
    g_ctx_frame.fetch_add(1, std::memory_order_relaxed);
    AddCtxSize(dcb);
    return HookAction::Continue;
}

// Entry of sub_8c860 (RenderBackend_Frame): it flips the RenderSingleFrameGPU and
// RenderSingleFrameCPU rings, once per frame, after every writer of the frame.
HookAction FrameFlipPre(GuestRegs&, void*) {
    EnsureGlobals();
    for (size_t i = 0; i < kGlobalCount; ++i) {
        if (kGlobals[i].derived) { // the two globals sub_8c860 flips
            g_flips[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    FinishFrame();
    SotcPcRing::OnFrameBoundary(); // sotc: SOTC_PC_RING frame epoch
    if (place::On()) { // sotc: SOTC_GPU_PLACE: ring expiry and the quarantine run per frame
        SotcGpuRegion::OnFrame(place::g_frame.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    return HookAction::Continue;
}

double Mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// The heap-id globals whose current slot equals slot, e.g. "RenderSingleFrameGPU,GameDoubleFrame". A slot
// that no global holds (or whose globals cannot be read) is "slot<N>": distinct per slot, so unresolved heaps
// stay apart (the old fallback "unnamed" merged all of them into one pool). The label is read when a granule
// is claimed; RelabelResolved renames the granules once the globals name their slot.
std::string SlotLabel(uint64_t slot) {
    std::string label;
    for (size_t i = 0; i < kGlobalCount; ++i) {
        uint32_t v = 0;
        if (ReadImageU32(g_global_off[i], &v) && v == slot) {
            if (!label.empty()) {
                label += ",";
            }
            label += kGlobals[i].name;
        }
    }
    if (label.empty()) {
        char unresolved[24];
        std::snprintf(unresolved, sizeof unresolved, "slot%llu", static_cast<unsigned long long>(slot));
        return unresolved;
    }
    return label;
}

// Registers a newly claimed granule in the granule table. The pool name is taken from the heap-id
// globals when the granule is claimed. The hash of that label is kept on the entry, so RelabelResolved can
// tell when a later read names the slot differently.
void RegisterGranuleOwner(GranuleEntry& entry, uint64_t granule_base, uint32_t slot) {
    if (g_classes_on) {
        const std::string label = SlotLabel(slot);
        const std::string pool = "memtype:" + label;
        entry.label.store(std::hash<std::string>{}(label), std::memory_order_release);
        SotcMemClasses::OnGranule(granule_base, kGranuleSize, pool.c_str());
    }
    if (SotcN1Verify::On()) { // sotc: SOTC_VERIFY=n1: the heap class of a missed bind
        SotcN1Verify::OnGranule(granule_base, kGranuleSize, ("memtype:" + SlotLabel(slot)).c_str());
    }
    // sotc: SOTC_PC_PLACE: the claim is a carve too, whether or not SOTC_MEMTYPE_CLASSES is on.
    SotcPcPlaceGlue::Global().OnHeapCarve(granule_base, static_cast<int32_t>(slot));
}

// Renames the claimed granules whose slot label changed after their claim (the heap-id globals were not set
// yet at the claim, so the pool was named "memtype:slot<N>" or another heap). Labels are read once per call;
// each registered granule's label hash is compared with its slot's current hash. Draw thread, from
// ReportMemtype; only SotcMemClasses::RelabelGranule takes a lock.
void RelabelResolved() {
    std::array<uint64_t, kSlots> hashes{};
    std::array<std::string, kSlots> labels;
    for (size_t s = 0; s < kSlots; ++s) {
        labels[s] = SlotLabel(s);
        hashes[s] = std::hash<std::string>{}(labels[s]);
    }
    for (GranuleEntry& e : g_granules) {
        const uint64_t w = e.word.load(std::memory_order_acquire);
        if (w == 0) {
            continue;
        }
        const uint32_t slot = static_cast<uint32_t>(w & 0xff);
        const uint64_t seen = e.label.load(std::memory_order_acquire);
        if (seen == 0 || seen == hashes[slot]) {
            continue; // not registered yet, or the claim already carries the current label
        }
        const uint64_t base = ((w >> 8) - 1) << kGranuleShift;
        SotcMemClasses::RelabelGranule(base, ("memtype:" + labels[slot]).c_str());
        e.label.store(hashes[slot], std::memory_order_release); // one try per label change
    }
}

// One granule touched by a call of slot. Lock-free unless the granule is new (then one CAS, and the
// owner registration of the claim path).
void NoteGranule(uint64_t gidx, uint32_t slot, uint64_t owner, uint64_t size) {
    const size_t start = static_cast<size_t>((gidx * 0x9E3779B97F4A7C15ull) >> (64 - kGranuleTableBits)) & (kGranuleTable - 1);
    const uint64_t want = ((gidx + 1) << 8) | slot;
    for (size_t probe = 0; probe < kGranuleTable; ++probe) {
        GranuleEntry& e = g_granules[(start + probe) & (kGranuleTable - 1)];
        uint64_t w = e.word.load(std::memory_order_acquire);
        if (w == 0) {
            // Claim a new granule: reserve a count first so the cap holds under concurrency.
            if (g_granule_claims.fetch_add(1, std::memory_order_relaxed) >= kMaxGranules) {
                g_granule_claims.fetch_sub(1, std::memory_order_relaxed);
                g_granule_overflow.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            uint64_t expected = 0;
            if (e.word.compare_exchange_strong(expected, want, std::memory_order_acq_rel)) {
                e.last_owner.store(owner, std::memory_order_relaxed);
                e.last_size.store(size, std::memory_order_relaxed);
                g_slot_granules[slot].fetch_add(1, std::memory_order_relaxed);
                RegisterGranuleOwner(e, gidx << kGranuleShift, slot);
                return;
            }
            g_granule_claims.fetch_sub(1, std::memory_order_relaxed); // another thread claimed this slot
            w = expected;
        }
        if ((w >> 8) == gidx + 1) {
            e.last_owner.store(owner, std::memory_order_relaxed); // sotc: SOTC_FAULT_OWNERS
            e.last_size.store(size, std::memory_order_relaxed);
            const uint32_t owner_slot = static_cast<uint32_t>(w & 0xff);
            if (owner_slot != slot) {
                const uint64_t bit = uint64_t{1} << slot;
                if ((e.foreign.load(std::memory_order_relaxed) & bit) == 0) {
                    const uint64_t prev = e.foreign.fetch_or(bit, std::memory_order_relaxed);
                    if ((prev & bit) == 0) { // sotc: SOTC_PC_PLACE: first carve of this heap here
                        SotcPcPlaceGlue::Global().OnHeapCarve(gidx << kGranuleShift, static_cast<int32_t>(slot));
                    }
                    if (prev == 0 && g_classes_on) {
                        g_granule_shared.fetch_add(1, std::memory_order_relaxed);
                        SotcMemClasses::OnGranuleShared(gidx << kGranuleShift); // never CPU_ONLY
                    }
                }
            }
            return;
        }
    }
    g_granule_overflow.fetch_add(1, std::memory_order_relaxed); // table full (cannot happen below the cap)
}

// The granules of one returned allocation. Spans past the claim cap are counted as overflow in O(1).
void NoteAllocGranules(uint64_t base, uint64_t size, uint32_t slot, uint64_t caller) {
    // Guest addresses below 2^48 and sizes below 2^48 (the bounds of SotcMemClasses::OnGranule).
    if (base == 0 || size == 0 || (base >> 48) != 0 || (size >> 48) != 0) {
        return;
    }
    const uint64_t first = base >> kGranuleShift;
    const uint64_t last = (base + size - 1) >> kGranuleShift;
    const uint64_t span = last - first + 1;
    const uint64_t walk = span < kMaxGranules ? span : kMaxGranules;
    const uint64_t owner = (uint64_t{slot} << 56) | (caller & ((uint64_t{1} << 48) - 1));
    for (uint64_t i = 0; i < walk; ++i) {
        NoteGranule(first + i, slot, owner, size);
    }
    if (span > walk) {
        g_granule_overflow.fetch_add(span - walk, std::memory_order_relaxed);
    }
}

// Post phase of MEMTYPE_ALLOC (registered with SOTC_MEMTYPE_CLASSES=1 or the placement glue): rax = returned pointer.
void AllocPost(GuestRegs& regs, void*) {
    if (t_pending_depth == 0) {
        return; // no matching pre phase on this thread (cannot happen while the hook is installed)
    }
    --t_pending_depth;
    const PendingAlloc call = t_pending[t_pending_depth % kPendingDepth];
    SotcPcPlaceGlue::PopHeapSlot(call.prev_heap); // sotc: SOTC_PC_PLACE heap context ends with the call
    if (g_post_granules) {
        NoteAllocGranules(regs.rax, call.size, call.slot, call.caller);
    }
    if (SotcAllocSites::On()) { // sotc: SOTC_ALLOC_SITES
        SotcAllocSites::NoteAlloc(regs.rax, call.size, call.slot, call.caller, call.caller2, call.caller3,
                                  call.trace);
    }
    // sotc: SOTC_NATIVE_OWNER_DISCARD: a fresh block's old contents are dead (freed, or a ring block reused with no
    // free), so its GPU-written state is forgotten before the new owner writes it. The render-target pool (slot 20)
    // is carved by the engine into surfaces the GPU keeps writing: excluded.
    if (SotcNative::OwnerDiscardOn() && regs.rax != 0 && call.size != 0) {
        if (SotcNative::OwnerDiscardAtFree()) {
            SotcAllocSites::CoreNoteAlloc(regs.rax, call.size);
        }
        if (call.slot != kRenderTargetSlot) {
            SotcNative::SetOwnerDiscardTag((static_cast<uint64_t>(call.slot & 0x7fff) << 48) |
                                           (call.caller & 0xffffffffffull));
            SotcNative::DiscardOwner(regs.rax, call.size);
        }
    }
    // sotc: SOTC_NATIVE: a RenderSingleFrameCPU block is announced PerFrame at the next submit.
    if (SotcNative::On() && static_cast<int64_t>(call.slot) == kRenderSingleFrameCpuSlot && regs.rax != 0) {
        SotcNativeHooks::NoteFrameBlock(regs.rax, call.size);
    }
    // sotc: SOTC_PC_RING_HEAPSTAT: where the allocation landed relative to the served ranges, per slot.
    SotcPcRing::NoteHeapAlloc(call.slot, regs.rax, call.size);
}

// sotc: SOTC_ALLOC_SITES: entry of the generic free 0x12af0 (MEMTYPE_FREE), rdi = the block 0x12820 returned.
HookAction FreePre(GuestRegs& regs, void*) {
    if (place::TakeFree(regs.rdi)) { // sotc: SOTC_GPU_PLACE: a region block never reaches the engine heap
        if (SotcAllocSites::On()) {
            SotcAllocSites::NoteFree(regs.rdi, SotcAllocSites::kFreeGeneric);
        }
        return HookAction::Return;
    }
    if (SotcAllocSites::On()) {
        SotcAllocSites::NoteFree(regs.rdi, SotcAllocSites::kFreeGeneric);
    }
    // sotc: SOTC_NATIVE_OWNER_DISCARD: the freed block's contents are dead, so its GPU-written state is forgotten
    // (the allocator's own writes into the free block, e.g. free-run headers, then hit an unprotected page).
    if (SotcNative::OwnerDiscardAtFree()) {
        if (const uint64_t size = SotcAllocSites::CoreTakeFree(regs.rdi); size != 0) {
            uint64_t ret = 0;
            std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof ret);
            uint64_t off = 0;
            SotcNative::SetOwnerDiscardTag((uint64_t{1} << 63) | (ImageOffset(ret, &off) ? off : 0));
            SotcNative::DiscardOwner(regs.rdi, size);
        }
    }
    return HookAction::Continue;
}

// Entries of the heap frees reached through vtbl+0xa8 (rdi = heap, rsi = the block): the small-object allocator
// 0x17e90 and the BPE heap 0x493d0. They see the frees that skip 0x12af0 (direct vtable calls, the realloc).
HookAction FreeFamilyPre(GuestRegs& regs, void*) { // sotc: SOTC_GPU_PLACE: the family free 0x47f30
    return place::TakeFree(regs.rsi) ? HookAction::Return : HookAction::Continue;
}

HookAction FreeSmallPre(GuestRegs& regs, void*) {
    if (place::TakeFree(regs.rsi)) { // sotc: SOTC_GPU_PLACE (a direct vtable free of a region block)
        return HookAction::Return;
    }
    if (SotcAllocSites::On()) {
        SotcAllocSites::NoteFree(regs.rsi, SotcAllocSites::kFreeSmall);
    }
    return HookAction::Continue;
}
HookAction FreeBpePre(GuestRegs& regs, void*) {
    if (place::TakeFree(regs.rsi)) { // sotc: SOTC_GPU_PLACE (a direct vtable free of a region block)
        return HookAction::Return;
    }
    if (SotcAllocSites::On()) {
        SotcAllocSites::NoteFree(regs.rsi, SotcAllocSites::kFreeBpe);
    }
    return HookAction::Continue;
}

// Wrap of the realloc 0x12c50(ptr, size, align, flags): it allocates through the heap's vtbl+0xa0 and frees the old
// block through vtbl+0xa8, so neither 0x12820 nor 0x12af0 sees the new block. Pre: rdi = old block, rsi = new size.
struct PendingRealloc {
    uint64_t old_addr, size;
    uint32_t site, trace;
};
thread_local std::array<PendingRealloc, kPendingDepth> t_realloc{};
thread_local uint32_t t_realloc_depth = 0;
HookAction ReallocPre(GuestRegs& regs, void*) {
    // sotc: SOTC_GPU_PLACE: a region block is reallocated inside the region (the engine heap does not know it).
    if (place::On() && SotcGpuRegion::Contains(regs.rdi)) {
        const uint64_t old_addr = regs.rdi;
        const uint64_t new_size = regs.rsi;
        const uint64_t old_size = SotcGpuRegion::SizeOf(old_addr);
        uint32_t slot = 0;
        SotcGpuRegion::SlotOf(old_addr, &slot);
        place::g_reallocs.fetch_add(1, std::memory_order_relaxed);
        uint64_t new_addr = 0;
        if (new_size != 0 && old_size != 0) {
            new_addr = SotcGpuRegion::Alloc(new_size, 16, slot, SotcGpuRegion::kKindNormal);
            if (new_addr != 0) {
                std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(new_addr)),
                            reinterpret_cast<const void*>(static_cast<uintptr_t>(old_addr)),
                            std::min(old_size, new_size));
            }
        }
        if (new_size == 0 || new_addr != 0) {
            SotcGpuRegion::Free(old_addr);
        }
        regs.rax = new_addr;
        return HookAction::Return;
    }
    PendingRealloc& p = t_realloc[t_realloc_depth % kPendingDepth];
    p.old_addr = regs.rdi;
    p.size = regs.rsi;
    p.site = ~0u;
    p.trace = ~0u;
    if (SotcAllocSites::On()) {
        SotcAllocSites::ReallocPre(regs.rdi, &p.site, &p.trace);
    }
    ++t_realloc_depth;
    return HookAction::Continue;
}
void ReallocPost(GuestRegs& regs, void*) {
    if (t_realloc_depth == 0) {
        return;
    }
    --t_realloc_depth;
    const PendingRealloc p = t_realloc[t_realloc_depth % kPendingDepth];
    if (SotcAllocSites::On()) {
        SotcAllocSites::ReallocPost(p.old_addr, regs.rax, p.size, p.site, p.trace);
    }
}

// sotc: SOTC_GPU_PLACE: entry of the pointer classifier 0x19a50(ptr). A region pointer has no pool class byte; it is
// answered with the heap object its site asked for (0 for a block that is no longer live, as for an unknown pointer).
HookAction ClassifyPre(GuestRegs& regs, void*) {
    if (!place::On() || !SotcGpuRegion::Contains(regs.rdi)) {
        return HookAction::Continue;
    }
    uint32_t slot = 0;
    uint64_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof ret);
    uint64_t ret_off = 0;
    if (!SotcGpuRegion::SlotOf(regs.rdi, &slot)) {
        regs.rax = 0;
    } else if (ImageOffset(ret, &ret_off) && ret_off == place::kFamilyClassifyRet) {
        regs.rax = place::StandInHeapAddress(); // the family free forwards to its vtbl+0xa8: 0x17e90 (hooked)
    } else {
        regs.rax = place::HeapOf(slot);
    }
    place::g_classified.fetch_add(1, std::memory_order_relaxed);
    return HookAction::Return;
}

// Granule summary of the classes mode. The counts are totals (granules are never freed), so the line
// repeats every window while the mode is on.
void ReportGranules(std::string& out) {
    if (!g_classes_on) {
        return;
    }
    const uint64_t shared = g_granule_shared.load(std::memory_order_relaxed);
    const uint64_t overflow = g_granule_overflow.load(std::memory_order_relaxed);
    std::string per_slot;
    uint64_t total = 0;
    for (size_t s = 0; s < kSlots; ++s) {
        const uint64_t n = g_slot_granules[s].load(std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        total += n;
        char item[160];
        std::snprintf(item, sizeof item, "%s%zu [%s] %llu", per_slot.empty() ? "" : ", ", s,
                      SlotLabel(s).c_str(), static_cast<unsigned long long>(n));
        per_slot += item;
    }
    if (total == 0 && shared == 0 && overflow == 0) {
        return;
    }
    // Built as a string: the per-slot list has up to 64 entries.
    char head[160];
    std::snprintf(head, sizeof head, "SOTC MEMTYPE CLASSES: granules %llu of %zu (per slot: ",
                  static_cast<unsigned long long>(total), kMaxGranules);
    char tail[96];
    std::snprintf(tail, sizeof tail, "); shared %llu; overflow %llu granule touches\n",
                  static_cast<unsigned long long>(shared), static_cast<unsigned long long>(overflow));
    out += head;
    out += per_slot.empty() ? std::string("none") : per_slot;
    out += tail;
}

// sotc: SOTC_FAULT_OWNERS=1 (with SOTC_HOOK_MEMTYPE_PROBE=1 SOTC_MEMTYPE_CLASSES=1 and SOTC_NATIVE_TRACK=3).
// Every write fault on a GPU-written page is charged to the last allocation that touched its 64 KiB granule
// (heap slot and the allocation's caller), to tell which allocations hold data both the GPU and the CPU write.
struct FaultOwnerBin {
    std::atomic<uint64_t> key{0}; // owner + 1 (0 = empty)
    std::atomic<uint64_t> faults{0};
    std::atomic<uint64_t> size{0};
    std::atomic<uint64_t> addr{0}; // the last fault address
    // Age of the page's GPU-written mark at the fault: < 100 ms, < 1 s, < 10 s, >= 10 s, unknown (no stamp).
    std::array<std::atomic<uint64_t>, 5> age{};
};

// Per 4 KiB page, the time (ms) of the last GPU write that marked it (direct-mapped by page; a collision loses
// the older page, which then reads as "unknown").
constexpr size_t kStampBits = 22;
struct PageStamp {
    std::atomic<uint64_t> page{0}; // page index + 1
    std::atomic<uint64_t> ms{0};
};
std::array<PageStamp, size_t{1} << kStampBits> g_stamps{};
uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
size_t StampSlot(uint64_t page) {
    return static_cast<size_t>((page * 0x9E3779B97F4A7C15ull) >> (64 - kStampBits));
}
constexpr size_t kFaultOwnerBins = 1024;
std::array<FaultOwnerBin, kFaultOwnerBins> g_fault_owners{};
std::atomic<uint64_t> g_fault_owner_total{0};
std::atomic<uint64_t> g_fault_owner_unclaimed{0};
std::atomic<uint64_t> g_fault_owner_overflow{0};

const GranuleEntry* FindGranule(uint64_t gidx) {
    const size_t start = static_cast<size_t>((gidx * 0x9E3779B97F4A7C15ull) >> (64 - kGranuleTableBits)) & (kGranuleTable - 1);
    for (size_t probe = 0; probe < kGranuleTable; ++probe) {
        const GranuleEntry& e = g_granules[(start + probe) & (kGranuleTable - 1)];
        const uint64_t w = e.word.load(std::memory_order_acquire);
        if (w == 0) {
            return nullptr;
        }
        if ((w >> 8) == gidx + 1) {
            return &e;
        }
    }
    return nullptr;
}

std::string CallerText(uint64_t key) {
    if (key == kOutside) {
        return "outside";
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(key));
    return buf;
}

struct Caller {
    uint64_t key = 0;
    uint64_t calls = 0;
    uint64_t bytes = 0;
};

struct SlotSnap {
    size_t index = 0;
    uint64_t calls = 0;
    uint64_t bytes = 0;
    uint64_t frame_sum = 0;
    uint64_t frame_max = 0;
    uint64_t overflow = 0;
    std::vector<Caller> callers;
};

} // namespace

void RegisterMemtype() {
    // Registered unconditionally: a hook that is off never dispatches to its handler.
    // MEMTYPE_FLIP_SITE (entry at sub_8c860) supplies the derived addresses and the frame boundary.
    // SOTC_MEMTYPE_CLASSES: a non-zero integer turns on the granule claims (post phase of MEMTYPE_ALLOC).
    // Read once here; off, the post phase is not registered and the hook costs what it did before.
    const char* classes = std::getenv("SOTC_MEMTYPE_CLASSES");
    g_classes_on = classes && *classes && std::strtol(classes, nullptr, 10) != 0;
    // sotc: SOTC_PC_PLACE needs the same post phase (granule carves and the heap context), with or without classes.
    g_post_granules = g_classes_on || SotcPcPlaceGlue::On();
    // sotc: SOTC_NATIVE needs the post phase too (NoteFrameBlock runs there); the post phase then runs without the
    // granule claims unless the classes mode or the glue asks for them (g_post_granules).
    g_post_on = g_post_granules || SotcNative::On() || SotcPcRing::HeapStatOn() || SotcAllocSites::On() ||
                SotcAllocSites::CoreOn(); // SOTC_NATIVE_OWNER_DISCARD (env only: the native mode is set later)

    SotcHooks::Handler alloc;
    alloc.pre = &AllocPre;
    if (g_post_on) {
        alloc.post = &AllocPost;
    }
    SotcHooks::RegisterHandler("MEMTYPE_ALLOC", alloc);

    SotcHooks::Handler free_entry; // sotc: SOTC_ALLOC_SITES ends allocations at their free
    free_entry.pre = &FreePre;
    SotcHooks::RegisterHandler("MEMTYPE_FREE", free_entry);
    SotcHooks::Handler free_small;
    free_small.pre = &FreeSmallPre;
    SotcHooks::RegisterHandler("MEMTYPE_FREE_SMALL", free_small);
    SotcHooks::Handler free_bpe;
    free_bpe.pre = &FreeBpePre;
    SotcHooks::RegisterHandler("MEMTYPE_FREE_BPE", free_bpe);
    SotcHooks::Handler free_family;
    free_family.pre = &FreeFamilyPre;
    SotcHooks::RegisterHandler("MEMTYPE_FREE_FAMILY", free_family);
    SotcHooks::Handler classify;
    classify.pre = &ClassifyPre;
    SotcHooks::RegisterHandler("MEMTYPE_CLASSIFY", classify);
    SotcHooks::Handler realloc_wrap;
    realloc_wrap.pre = &ReallocPre;
    realloc_wrap.post = &ReallocPost;
    SotcHooks::RegisterHandler("MEMTYPE_REALLOC", realloc_wrap);

    SotcHooks::Handler ctx;
    ctx.pre = &RenderCtxPre;
    SotcHooks::RegisterHandler("RENDER_CTX_REQUEST", ctx);

    SotcHooks::Handler flip;
    flip.pre = &FrameFlipPre;
    SotcHooks::RegisterHandler("MEMTYPE_FLIP_SITE", flip);
}

void ReportMemtype(std::string& out, double seconds) {
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    place::Report(out); // sotc: SOTC_GPU_PLACE
    EnsureGlobals();
    if (g_classes_on) {
        RelabelResolved(); // granules claimed before the heap-id globals were set take their name now
    }
    // sotc: SOTC_FAULT_OWNERS: the top owners of the window's write faults.
    if (const uint64_t total = g_fault_owner_total.exchange(0, std::memory_order_relaxed); total != 0) {
        struct Row {
            uint64_t owner;
            uint64_t faults;
            uint64_t size;
            uint64_t addr;
            std::array<uint64_t, 5> age;
        };
        std::vector<Row> rows;
        std::array<uint64_t, 5> age_total{};
        for (FaultOwnerBin& b : g_fault_owners) {
            const uint64_t k = b.key.load(std::memory_order_relaxed);
            const uint64_t n = b.faults.exchange(0, std::memory_order_relaxed);
            std::array<uint64_t, 5> age{};
            for (size_t i = 0; i < 5; ++i) {
                age[i] = b.age[i].exchange(0, std::memory_order_relaxed);
                age_total[i] += age[i];
            }
            if (k != 0 && n != 0) {
                rows.push_back(Row{k - 1, n, b.size.load(std::memory_order_relaxed),
                                   b.addr.load(std::memory_order_relaxed), age});
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.faults > b.faults; });
        std::string text;
        for (size_t i = 0; i < rows.size() && i < 16; ++i) {
            const uint64_t slot = rows[i].owner >> 56;
            const uint64_t caller = rows[i].owner & ((uint64_t{1} << 48) - 1);
            const std::string who = caller == (kOutside & ((uint64_t{1} << 48) - 1)) ? std::string{"outside"}
                                                                                      : CallerText(caller);
            char item[320];
            std::snprintf(item, sizeof item,
                          "%s%s %s x%llu (last alloc 0x%llx, fault 0x%llx; mark age <0.1s %llu <1s %llu <10s %llu "
                          ">=10s %llu ? %llu)",
                          text.empty() ? "" : "; ", SlotLabel(slot).c_str(), who.c_str(),
                          static_cast<unsigned long long>(rows[i].faults),
                          static_cast<unsigned long long>(rows[i].size),
                          static_cast<unsigned long long>(rows[i].addr),
                          static_cast<unsigned long long>(rows[i].age[0]),
                          static_cast<unsigned long long>(rows[i].age[1]),
                          static_cast<unsigned long long>(rows[i].age[2]),
                          static_cast<unsigned long long>(rows[i].age[3]),
                          static_cast<unsigned long long>(rows[i].age[4]));
            text += item;
        }
        char head[192];
        std::snprintf(head, sizeof head,
                      "SOTC FAULT OWNERS (window): %llu write faults, %llu outside claimed granules, %llu owners not "
                      "counted (table full); ",
                      static_cast<unsigned long long>(total),
                      static_cast<unsigned long long>(g_fault_owner_unclaimed.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_fault_owner_overflow.exchange(0, std::memory_order_relaxed)));
        out += head;
        char ages[160];
        std::snprintf(ages, sizeof ages,
                      "mark age of all: <0.1s %llu, <1s %llu, <10s %llu, >=10s %llu, unknown %llu; ",
                      static_cast<unsigned long long>(age_total[0]), static_cast<unsigned long long>(age_total[1]),
                      static_cast<unsigned long long>(age_total[2]), static_cast<unsigned long long>(age_total[3]),
                      static_cast<unsigned long long>(age_total[4]));
        out += ages;
        out += text;
        out += "\n";
    }
    ReportGranules(out); // cumulative, printed every window while SOTC_MEMTYPE_CLASSES=1

    // Snapshot the window and reset its counters. Frame totals in flight (frame_bytes) carry over.
    std::vector<SlotSnap> slots;
    for (size_t i = 0; i < kSlots; ++i) {
        SlotStats& s = g_slots[i];
        SlotSnap snap;
        snap.index = i;
        snap.calls = s.calls.exchange(0, std::memory_order_relaxed);
        snap.bytes = s.bytes.exchange(0, std::memory_order_relaxed);
        snap.frame_sum = s.frame_sum.exchange(0, std::memory_order_relaxed);
        snap.frame_max = s.frame_max.exchange(0, std::memory_order_relaxed);
        snap.overflow = s.caller_overflow.exchange(0, std::memory_order_relaxed);
        for (CallerBin& c : s.callers) {
            const uint64_t key = c.key.load(std::memory_order_acquire);
            const uint64_t calls = c.calls.exchange(0, std::memory_order_relaxed);
            const uint64_t bytes = c.bytes.exchange(0, std::memory_order_relaxed);
            if (key != 0 && calls != 0) {
                snap.callers.push_back(Caller{key, calls, bytes});
            }
        }
        if (snap.calls != 0) {
            slots.push_back(std::move(snap));
        }
    }

    std::array<uint64_t, kGlobalCount + 1> flips{};
    for (size_t i = 0; i <= kGlobalCount; ++i) {
        flips[i] = g_flips[i].exchange(0, std::memory_order_relaxed);
    }
    uint64_t flip_total = 0;
    for (uint64_t f : flips) {
        flip_total += f;
    }
    const uint64_t frames = flips[kFrameGlobal];

    const uint64_t requests = g_ctx_requests.exchange(0, std::memory_order_relaxed);
    const uint64_t ctx_sum = g_ctx_sum.exchange(0, std::memory_order_relaxed);
    const uint64_t ctx_max = g_ctx_max.exchange(0, std::memory_order_relaxed);
    const uint64_t ctx_overflow = g_ctx_size_overflow.exchange(0, std::memory_order_relaxed);
    struct SizeSnap {
        uint64_t size;
        uint64_t count;
    };
    std::vector<SizeSnap> sizes;
    for (SizeBin& b : g_ctx_sizes) {
        const uint64_t size = b.size.load(std::memory_order_acquire);
        const uint64_t count = b.count.exchange(0, std::memory_order_relaxed);
        if (size != 0 && count != 0) {
            sizes.push_back(SizeSnap{size, count});
        }
    }
    const uint64_t clamped = g_slot_clamped.exchange(0, std::memory_order_relaxed);

    // Idle window: nothing printed.
    if (slots.empty() && requests == 0 && flip_total == 0 && clamped == 0) {
        return;
    }

    char line[1024]; // eight callers of up to ~80 characters each
    const double window = seconds;

    for (SlotSnap& snap : slots) {
        std::sort(snap.callers.begin(), snap.callers.end(),
                  [](const Caller& a, const Caller& b) { return a.bytes > b.bytes; });
        std::string callers;
        for (size_t k = 0; k < snap.callers.size() && k < kTopCallers; ++k) {
            const Caller& c = snap.callers[k];
            char item[96];
            std::snprintf(item, sizeof item, "%s%s %.2f MiB (%llu calls)", k ? ", " : "",
                          CallerText(c.key).c_str(), Mib(c.bytes),
                          static_cast<unsigned long long>(c.calls));
            callers += item;
        }
        if (callers.empty()) {
            callers = "none";
        }
        std::string per_frame_avg = "n/a";
        std::string per_frame_max = "n/a";
        if (frames != 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.3f MiB", Mib(snap.frame_sum / frames));
            per_frame_avg = buf;
            std::snprintf(buf, sizeof buf, "%.3f MiB", Mib(snap.frame_max));
            per_frame_max = buf;
        }
        std::snprintf(line, sizeof line,
                      "SOTC MEMTYPE slot %zu [%s]: %.0f calls/s, %.2f MiB/s, bytes/frame avg %s max %s, "
                      "callers %s%s\n",
                      snap.index, SlotLabel(snap.index).c_str(), static_cast<double>(snap.calls) / window,
                      Mib(snap.bytes) / window, per_frame_avg.c_str(), per_frame_max.c_str(),
                      callers.c_str(),
                      snap.overflow ? " (caller table full: more callers not listed)" : "");
        out += line;
    }
    if (clamped != 0) {
        std::snprintf(line, sizeof line,
                      "SOTC MEMTYPE: %llu allocations with a memtype slot >= %zu counted in slot %zu\n",
                      static_cast<unsigned long long>(clamped), kSlots, kSlots - 1);
        out += line;
    }

    if (requests != 0) {
        std::string avg = "n/a";
        if (frames != 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(ctx_sum) / static_cast<double>(frames));
            avg = buf;
        }
        std::string size_text;
        for (const SizeSnap& s : sizes) {
            char item[48];
            std::snprintf(item, sizeof item, "%s0x%llx x%llu", size_text.empty() ? "" : ", ",
                          static_cast<unsigned long long>(s.size), static_cast<unsigned long long>(s.count));
            size_text += item;
        }
        if (ctx_overflow != 0) {
            char item[48];
            std::snprintf(item, sizeof item, ", other x%llu", static_cast<unsigned long long>(ctx_overflow));
            size_text += item;
        }
        std::snprintf(line, sizeof line,
                      "SOTC RENDERCTX: %llu chunks requested (%.1f/s) in %.0f s over %llu frames; "
                      "chunks/frame avg %s max %llu; DCB sizes %s\n",
                      static_cast<unsigned long long>(requests), static_cast<double>(requests) / window,
                      window, static_cast<unsigned long long>(frames), avg.c_str(),
                      static_cast<unsigned long long>(ctx_max), size_text.empty() ? "none" : size_text.c_str());
        out += line;
    }

    if (flip_total != 0) {
        std::string items;
        for (size_t i = 0; i < kGlobalCount; ++i) {
            if (flips[i] == 0) {
                continue;
            }
            uint32_t slot = 0;
            char slot_text[16];
            if (ReadImageU32(g_global_off[i], &slot)) {
                std::snprintf(slot_text, sizeof slot_text, "%u", slot);
            } else {
                std::snprintf(slot_text, sizeof slot_text, "?");
            }
            char item[128];
            std::snprintf(item, sizeof item, "%s%s (slot %s) %llu", items.empty() ? "" : ", ",
                          kGlobals[i].name, slot_text, static_cast<unsigned long long>(flips[i]));
            items += item;
        }
        if (flips[kOtherGlobal] != 0) {
            char item[48];
            std::snprintf(item, sizeof item, "%sother %llu", items.empty() ? "" : ", ",
                          static_cast<unsigned long long>(flips[kOtherGlobal]));
            items += item;
        }
        std::snprintf(line, sizeof line, "SOTC RINGFLIP: %.0f s, frames %llu; flips %s\n", window,
                      static_cast<unsigned long long>(frames), items.c_str());
        out += line;
    }
}

bool FaultOwnersOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_FAULT_OWNERS");
        return v && v[0] == '1';
    }();
    return on;
}

void NoteGpuWriteStamp(uint64_t addr, uint64_t size) {
    if (size == 0) {
        return;
    }
    const uint64_t now = NowMs();
    const uint64_t first = addr >> 12;
    const uint64_t last = (addr + size - 1) >> 12;
    const uint64_t pages = std::min<uint64_t>(last - first + 1, 4096); // 16 MiB at most per write
    for (uint64_t p = first; p < first + pages; ++p) {
        PageStamp& st = g_stamps[StampSlot(p)];
        st.page.store(p + 1, std::memory_order_relaxed);
        st.ms.store(now, std::memory_order_relaxed);
    }
}

void NoteFaultOwner(uint64_t addr) {
    g_fault_owner_total.fetch_add(1, std::memory_order_relaxed);
    const GranuleEntry* e = FindGranule(addr >> kGranuleShift);
    if (!e) {
        g_fault_owner_unclaimed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t owner = e->last_owner.load(std::memory_order_relaxed);
    const uint64_t key = owner + 1;
    size_t age_bucket = 4;
    {
        const uint64_t page = addr >> 12;
        const PageStamp& st = g_stamps[StampSlot(page)];
        if (st.page.load(std::memory_order_relaxed) == page + 1) {
            const uint64_t age = NowMs() - st.ms.load(std::memory_order_relaxed);
            age_bucket = age < 100 ? 0 : age < 1000 ? 1 : age < 10000 ? 2 : 3;
        }
    }
    const size_t start = static_cast<size_t>((owner * 0x9E3779B97F4A7C15ull) >> 54) & (kFaultOwnerBins - 1);
    for (size_t probe = 0; probe < kFaultOwnerBins; ++probe) {
        FaultOwnerBin& b = g_fault_owners[(start + probe) & (kFaultOwnerBins - 1)];
        uint64_t k = b.key.load(std::memory_order_acquire);
        if (k == 0) {
            uint64_t expected = 0;
            if (b.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                k = key;
            } else {
                k = expected;
            }
        }
        if (k == key) {
            b.faults.fetch_add(1, std::memory_order_relaxed);
            b.age[age_bucket].fetch_add(1, std::memory_order_relaxed);
            b.size.store(e->last_size.load(std::memory_order_relaxed), std::memory_order_relaxed);
            b.addr.store(addr, std::memory_order_relaxed);
            return;
        }
    }
    g_fault_owner_overflow.fetch_add(1, std::memory_order_relaxed);
}

} // namespace SotcProbes
