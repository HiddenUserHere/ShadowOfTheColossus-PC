// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: block-allocator probe (plan step 0c, docs/HOOK-DESIGN.md). Two hooks (scripts/hooks.d/alloc.py),
// switch SOTC_HOOK_ALLOC_PROBE:
//   ALLOC_BLOCK  wrap at mem_allocator_alloc (0x479a0). Pre: rdi = owner, the memory-family object
//                (name char* at +0x10, kind int32 at +0x18: 1 = CPU, 2 = GPU), rsi = requested size,
//                rdx = alignment and rcx = kind, both forwarded to the pool. Post: rax = block node (0 on
//                failure); node base at +0x60, size at +0x58, pool slot byte at +0x68 (0 flexible,
//                1 Onion, 2 Garlic).
//   FREE_BLOCK   entry at pool_free_block (0x1b0e0). rdi = the node ALLOC_BLOCK returned; size at +0x58.
// The probe aggregates per allocator name + kind (pool rows) and per owner object (owner rows, keyed by
// the family pointer, because the same names exist for kind 1 and kind 2 objects). An owner row counts
// the window allocations, their slot histogram and spills: a block from a pool set the owner's kind does
// not expect (kind 1 expects slots 0 and 1; kinds 0 and 2 expect slot 2). It also counts VA reuse: an
// allocation at a base freed earlier, looked up in a bounded direct-mapped table of freed bases.
// A free carries only the node, so each live block's pool, owner and size are remembered in a fixed node
// table and looked up when it is freed. Every tracked block is also fed to SotcMemClasses::OnAllocOwner
// (and freed through SotcMemClasses::OnFree). Observe only: every handler returns HookAction::Continue
// and nothing in the guest is written.
//
// Guest reads go through the backing view (runtime_memory_backing_pointer / runtime_memory_region),
// never the guest mapping, which the GPU page tracker may protect. Addresses inside the loaded game
// image (static allocator objects, name strings) are read directly: the image is not in that view.

#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "hooks.h"
#include "rb_class.h"     // sotc: SOTC_RB_CLASS feed (XPR mask arena)
#include "pc_pool.h"      // sotc: SOTC_PC_POOL=2 free feed
#include "pc_ring.h"     // sotc: SOTC_PC_RING feed (per-frame arena blocks)
#include "pc_pool_place_glue.h" // sotc: SOTC_PC_PLACE / SOTC_PC_RECYCLE feed
#include "mem_classes.h" // sotc: step 6 feed (CPU-only ranges) and the PC memory model owner feed
#include "native/n1verify.h" // sotc: SOTC_VERIFY=n1 allocator class feed
#include "native/native_draw.h" // sotc: SOTC_NATIVE_FREE_DISCARD
#include "probes.h"
#include "video_core/sotc_trace.h" // image bounds (SotcTrace::ImageBase/ImageSize)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kNameOffset = 0x10; // family object: char* allocator name
constexpr uint64_t kKindOffset = 0x18; // family object: int32 kind (1 CPU, 2 GPU)
constexpr uint64_t kNodeSizeOffset = 0x58; // block node: allocated size
constexpr uint64_t kNodeBaseOffset = 0x60; // block node: guest base address of the allocation
constexpr uint64_t kNodeSlotOffset = 0x68; // block node: pool set (0 flexible, 1 Onion, 2 Garlic)
constexpr uint64_t kAlign64K = 0x10000;    // the alignment the call sites pass (rdx)

constexpr size_t kNameMax = 48;   // stored allocator name length (NUL-terminated)
constexpr size_t kMaxPools = 48;  // distinct name+kind rows
constexpr size_t kMaxOwners = 128; // distinct owner objects; later ones are fed but not aggregated
constexpr uint32_t kNoOwner = 0xffffffffu;
// Live blocks tracked (open addressing, power of two). The game keeps tens of thousands of blocks
// live (~300 more per second during loads), so 8192 filled in seconds.
constexpr size_t kMaxNodes = 65536;
constexpr int kMaxDepth = 8;      // nested ALLOC_BLOCK calls per thread (defensive; it does not nest)
// Direct-mapped table of recently freed block bases (0 = empty). A newer free overwrites a colliding
// older one, so the reuse count is a lower bound. 8192 slots = 64 KiB.
constexpr unsigned kFreedBits = 13;
constexpr size_t kFreedSlots = size_t{1} << kFreedBits;
constexpr int kSlotBuckets = 4; // histogram: slot 0, 1, 2, and 3 for an unreadable or other slot

// Reads n bytes of guest memory through the backing view, bounded to the mapped region so a read near
// a region edge cannot run past it. False when unmapped or partly outside the region.
bool ReadGuest(uintptr_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || address + n > end) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

// sotc: the loaded game image (eboot + linked modules) is mapped directly and is not in the GPU
// backing view: the allocator families' static objects and the allocator name strings live there,
// so reads through the backing view alone came back empty (names "?", kinds unknown).
bool InImage(uintptr_t address, size_t n) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    return base != 0 && size != 0 && address >= base && n <= size && address - base <= size - n;
}

// Guest memory first through the backing view (heap memory the GPU tracker may protect), then the
// image directly. False when neither holds the whole range.
bool ReadAny(uintptr_t address, void* out, size_t n) {
    if (ReadGuest(address, out, n)) {
        return true;
    }
    if (InImage(address, n)) {
        std::memcpy(out, reinterpret_cast<const void*>(address), n);
        return true;
    }
    return false;
}

// Copies a NUL-terminated guest C string into out (always terminated), bounded to the mapped region
// (backing view) or to the image.
void ReadGuestString(uintptr_t address, char* out, size_t cap) {
    out[0] = '\0';
    if (!address || cap == 0) {
        return;
    }
    const unsigned char* p = nullptr;
    size_t avail = 0;
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (runtime_memory_region(address, &start, &end, &mapped) && mapped && address >= start) {
        p = runtime_memory_backing_pointer(address);
        avail = static_cast<size_t>(end - address);
    }
    if (!p && InImage(address, 1)) {
        p = reinterpret_cast<const unsigned char*>(address);
        avail = static_cast<size_t>(SotcTrace::ImageBase() + SotcTrace::ImageSize() - address);
    }
    if (!p) {
        return;
    }
    size_t limit = std::min(cap - 1, avail);
    size_t i = 0;
    for (; i < limit && p[i] != 0; ++i) {
        out[i] = static_cast<char>(p[i]);
    }
    out[i] = '\0';
}

// One allocator pool, keyed by name + kind. Live/peak are lifetime; the w_* fields are the current
// report window and are reset by ReportAlloc.
struct PoolRow {
    char name[kNameMax] = {0};
    int32_t kind = 0;
    uint64_t alloc_count = 0; // lifetime allocations
    int64_t live_bytes = 0;   // lifetime allocations minus frees (clamped at 0)
    uint64_t peak_bytes = 0;  // lifetime peak of live_bytes
    uint64_t w_allocs = 0;    // window: allocations
    uint64_t w_frees = 0;     // window: frees
    uint64_t w_alloc_bytes = 0;
    uint64_t w_free_bytes = 0;
};

// One owner object (the family object an ALLOC_BLOCK call is made on), keyed by its address. The
// w_* fields are the report window (every successful allocation with a row counts there); the
// lifetime count, live and peak only count blocks the node table tracks.
struct OwnerRow {
    bool used = false;
    uint64_t owner = 0; // family object guest address (rdi)
    char name[kNameMax] = {0};
    int32_t kind = 0;   // +0x18 at first sight
    uint64_t alloc_count = 0;
    int64_t live_bytes = 0;
    uint64_t peak_bytes = 0;
    uint64_t w_allocs = 0;
    uint64_t w_alloc_bytes = 0;
    uint64_t w_slot[kSlotBuckets] = {0, 0, 0, 0};
    uint64_t w_spills = 0;
};

// node pointer -> pool row, owner row and remembered size, so a free (which carries only the node) is
// attributed.
struct NodeEntry {
    uint64_t key = 0; // guest node pointer; 0 = empty slot
    uint32_t pool = 0;
    uint32_t owner = kNoOwner; // owner row index, kNoOwner when the owner table was full
    uint64_t size = 0;
    uint64_t base = 0; // guest base address (for the step-6 CPU-only range table)
};

// All of the below is guarded by g_mutex, which is never held while calling into the guest (the
// hooks run on game threads; the mutex only covers host-side bookkeeping and the OnAlloc feeds).
std::mutex g_mutex;
// sotc: SOTC_BLOCK_EXTENT. Every live tracked block by base (base -> size), for BlockRemaining on the draw thread.
std::shared_mutex g_ext_mutex;
std::map<uint64_t, uint64_t> g_ext;
std::array<PoolRow, kMaxPools> g_pools{};
size_t g_pool_count = 0;
std::array<OwnerRow, kMaxOwners> g_owners{};
size_t g_owner_count = 0;
std::array<NodeEntry, kMaxNodes> g_nodes{};
size_t g_node_live = 0;
std::array<uint64_t, kFreedSlots> g_freed{}; // recently freed block bases, direct-mapped

// Window counters for the whole probe.
uint64_t g_w_allocs = 0;        // successful ALLOC_BLOCK calls (tracked or not)
uint64_t g_w_reuse = 0;         // allocations at a base seen freed before
uint64_t g_w_spills = 0;        // allocations from a pool set the owner's kind does not expect
uint64_t g_w_kind_mismatch = 0; // forwarded kind (rcx) differs from the owner's kind (+0x18)
uint64_t g_w_odd_align = 0;     // alignment (rdx) other than 64 KiB
uint64_t g_w_owner_dropped = 0; // allocations whose owner did not fit the owner table

std::atomic<uint64_t> g_missed_frees{0}; // frees of a node we never recorded (pre-probe, or table full)
std::atomic<uint64_t> g_dropped_allocs{0}; // allocs we could not record (pool or node table full)
std::atomic<uint64_t> g_overflow{0};       // ALLOC_BLOCK nested past kMaxDepth

// Finds or creates the pool row for name+kind. Returns -1 when the table is full. Caller holds g_mutex.
int FindPool(const char* name, int32_t kind) {
    // An unreadable name is stored as "?"; compare with the same key, or every such allocation
    // opened a new row and filled the table in seconds.
    if (!name[0]) {
        name = "?";
    }
    for (size_t i = 0; i < g_pool_count; ++i) {
        if (g_pools[i].kind == kind && std::strncmp(g_pools[i].name, name, kNameMax) == 0) {
            return static_cast<int>(i);
        }
    }
    if (g_pool_count == kMaxPools) {
        return -1;
    }
    PoolRow& row = g_pools[g_pool_count];
    row = PoolRow{};
    std::snprintf(row.name, kNameMax, "%s", name);
    row.kind = kind;
    return static_cast<int>(g_pool_count++);
}

// Finds or creates the owner row for a family object. Returns -1 when the table is full. Caller holds
// g_mutex. Rows are never removed, so an index stays valid for the life of the process.
int FindOwner(uint64_t owner, const char* name, int32_t kind) {
    for (size_t i = 0; i < g_owner_count; ++i) {
        if (g_owners[i].owner == owner) {
            return static_cast<int>(i);
        }
    }
    if (g_owner_count == kMaxOwners) {
        return -1;
    }
    OwnerRow& row = g_owners[g_owner_count];
    row = OwnerRow{};
    row.used = true;
    row.owner = owner;
    std::snprintf(row.name, kNameMax, "%s", name[0] ? name : "?");
    row.kind = kind;
    return static_cast<int>(g_owner_count++);
}

constexpr size_t kNodeMask = kMaxNodes - 1;
static_assert((kMaxNodes & kNodeMask) == 0, "kMaxNodes must be a power of two");

// Index of a base in the freed-base table: the 64 KiB block number, multiplied to spread the bits.
size_t FreedSlot(uint64_t base) {
    return static_cast<size_t>(((base >> 16) * 0x9E3779B97F4A7C15ull) >> (64 - kFreedBits));
}

// Inserts a live node. Caller holds g_mutex. Returns false when the table is (near) full.
bool NodeInsert(const NodeEntry& value) {
    if (!value.key || g_node_live >= kMaxNodes - (kMaxNodes / 8)) {
        return false; // keep the table under 7/8 so lookups stay short
    }
    size_t i = static_cast<size_t>(value.key) & kNodeMask;
    for (size_t probe = 0; probe < kMaxNodes; ++probe) {
        NodeEntry& e = g_nodes[i];
        if (e.key == 0) {
            e = value;
            ++g_node_live;
            return true;
        }
        if (e.key == value.key) { // same node re-allocated without a free: overwrite
            e = value;
            return true;
        }
        i = (i + 1) & kNodeMask;
    }
    return false;
}

// Removes a live node, copying it to out. Caller holds g_mutex. False when not found.
bool NodeRemove(uint64_t key, NodeEntry* out) {
    if (!key) {
        return false;
    }
    size_t i = static_cast<size_t>(key) & kNodeMask;
    for (size_t probe = 0; probe < kMaxNodes; ++probe) {
        NodeEntry& e = g_nodes[i];
        if (e.key == 0) {
            return false;
        }
        if (e.key == key) {
            *out = e;
            e.key = 0;
            --g_node_live;
            // Backward-shift deletion keeps the table tombstone-free.
            size_t j = (i + 1) & kNodeMask;
            while (g_nodes[j].key != 0) {
                size_t home = static_cast<size_t>(g_nodes[j].key) & kNodeMask;
                // Move entry j back to i if i lies in [home, j) cyclically.
                bool movable = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
                if (movable) {
                    g_nodes[i] = g_nodes[j];
                    g_nodes[j].key = 0;
                    i = j;
                }
                j = (j + 1) & kNodeMask;
            }
            return true;
        }
        i = (i + 1) & kNodeMask;
    }
    return false;
}

// ALLOC_BLOCK is a wrap: pre carries the owner, the requested size, and the alignment and kind that
// the callee forwards to the pool, to post, which sees the node.
struct AllocFrame {
    uint64_t family = 0;   // rdi: owner object
    uint64_t req_size = 0; // rsi: requested size
    uint64_t align = 0;    // rdx: alignment (forwarded to the pool)
    int32_t fwd_kind = 0;  // rcx: kind (forwarded to the pool), low 32 bits
};
struct FrameStack {
    AllocFrame frames[kMaxDepth];
    int depth = 0;
    int overflow = 0;
};
thread_local FrameStack t_alloc;

HookAction AllocPre(GuestRegs& regs, void*) {
    FrameStack& s = t_alloc;
    if (s.depth >= kMaxDepth) {
        ++s.overflow;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    AllocFrame& f = s.frames[s.depth++];
    f.family = regs.rdi;
    f.req_size = regs.rsi;
    f.align = regs.rdx;
    f.fwd_kind = static_cast<int32_t>(static_cast<uint32_t>(regs.rcx));
    return HookAction::Continue;
}

// Pool set of a block from its node's slot byte: 0, 1 or 2, or -1 when unreadable or out of range.
int SlotOf(uint64_t node) {
    uint8_t raw = 0xff;
    if (!ReadAny(node + kNodeSlotOffset, &raw, sizeof raw) || raw > 2) {
        return -1;
    }
    return raw;
}

int SlotBucket(int slot) {
    return slot >= 0 ? slot : kSlotBuckets - 1;
}

// True when the block came from a pool set the owner's kind does not expect. Kind 1 tries flexible (0)
// and then Onion (1), so Garlic (2) is a spill. Kinds 0 and 2 try Garlic first, so 0 and 1 are spills
// (report 06 section 2.1). Other kinds, and an unreadable slot, are never counted.
bool IsSpill(int32_t kind, int slot) {
    if (slot < 0) {
        return false;
    }
    if (kind == 1) {
        return slot == 2;
    }
    if (kind == 0 || kind == 2) {
        return slot != 2;
    }
    return false;
}

void AllocPost(GuestRegs& regs, void*) {
    FrameStack& s = t_alloc;
    if (s.overflow > 0) {
        --s.overflow;
        return;
    }
    if (s.depth == 0) {
        return;
    }
    const AllocFrame f = s.frames[--s.depth];

    const uint64_t node = regs.rax;
    if (!node) {
        return; // allocation failed: nothing live
    }
    char name[kNameMax] = {0};
    uint64_t name_ptr = 0;
    if (ReadAny(f.family + kNameOffset, &name_ptr, sizeof name_ptr)) {
        ReadGuestString(static_cast<uintptr_t>(name_ptr), name, kNameMax);
    }
    int32_t kind = 0;
    ReadAny(f.family + kKindOffset, &kind, sizeof kind);

    uint64_t size = f.req_size; // fall back to the requested size if the node field is unreadable
    uint64_t node_size = 0;
    if (ReadAny(node + kNodeSizeOffset, &node_size, sizeof node_size)) {
        size = node_size;
    }
    uint64_t base = 0;
    ReadAny(node + kNodeBaseOffset, &base, sizeof base);
    const int slot = SlotOf(node);

    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_w_allocs;
    if (f.align != kAlign64K) {
        ++g_w_odd_align;
    }
    if (f.fwd_kind != kind) {
        ++g_w_kind_mismatch;
    }
    if (base) {
        uint64_t& freed = g_freed[FreedSlot(base)];
        if (freed == base) {
            ++g_w_reuse;
            freed = 0;
        }
    }
    const bool spill = IsSpill(kind, slot);
    if (spill) {
        ++g_w_spills;
    }

    // Owner window counts: every successful allocation of a row the owner table holds.
    const int owner = FindOwner(f.family, name, kind);
    if (owner < 0) {
        ++g_w_owner_dropped;
    } else {
        OwnerRow& o = g_owners[owner];
        o.w_allocs += 1;
        o.w_alloc_bytes += size;
        o.w_slot[SlotBucket(slot)] += 1;
        if (spill) {
            o.w_spills += 1;
        }
    }

    const int pool = FindPool(name, kind);
    if (pool < 0) {
        g_dropped_allocs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    NodeEntry entry;
    entry.key = node;
    entry.pool = static_cast<uint32_t>(pool);
    entry.owner = owner < 0 ? kNoOwner : static_cast<uint32_t>(owner);
    entry.size = size;
    entry.base = base;
    if (!NodeInsert(entry)) {
        g_dropped_allocs.fetch_add(1, std::memory_order_relaxed);
        return; // do not count an alloc whose matching free we could not track
    }
    if (base != 0 && size != 0 && BlockExtentOn()) {
        std::unique_lock ext_lock{g_ext_mutex};
        g_ext[base] = size;
    }
    // sotc: step 6. A CPU-only range is a CPUMemory pool (kind 1) that is not a GPU upload pool.
    // "staging-*" are the upload staging pools, which the GPU reads, so they are never CPU-only.
    const bool cpu_only = kind == 1 && std::strncmp(name, "staging", 7) != 0;
    SotcMemClasses::OnAlloc(base, size, cpu_only, name);
    // sotc: PC memory model. Paired with the OnFree in FreePre, which runs for every tracked node.
    SotcMemClasses::OnAllocOwner(base, size, f.family, kind, name, slot);
    SotcPcPlaceGlue::Global().OnAllocObserved(SotcPcPlace::OwnerKey{f.family, name, kind}, base, size, slot, -1);
    SotcPcRing::OnBlock(base, size, name, kind); // sotc: SOTC_PC_RING, the per-frame arena blocks
    SotcRbClass::OnBlock(base, size, name, kind); // sotc: SOTC_RB_CLASS, the XPR mask arena
    SotcN1Verify::OnBlock(base, size, name, kind, slot); // sotc: SOTC_VERIFY=n1

    PoolRow& row = g_pools[pool];
    row.alloc_count += 1;
    row.live_bytes += static_cast<int64_t>(size);
    if (row.live_bytes > static_cast<int64_t>(row.peak_bytes)) {
        row.peak_bytes = static_cast<uint64_t>(row.live_bytes);
    }
    row.w_allocs += 1;
    row.w_alloc_bytes += size;

    if (owner >= 0) {
        OwnerRow& o = g_owners[owner];
        o.alloc_count += 1;
        o.live_bytes += static_cast<int64_t>(size);
        if (o.live_bytes > static_cast<int64_t>(o.peak_bytes)) {
            o.peak_bytes = static_cast<uint64_t>(o.live_bytes);
        }
    }
}

HookAction FreePre(GuestRegs& regs, void*) {
    const uint64_t node = regs.rdi;
    SotcPcPool::OnFreeNode(node); // sotc: SOTC_PC_POOL=2 free check (reads the node; before the lock)
    std::lock_guard<std::mutex> lock(g_mutex);
    NodeEntry e;
    if (!NodeRemove(node, &e)) {
        g_missed_frees.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    SotcMemClasses::OnFree(e.base); // sotc: step 6 (no-op for ranges it never tracked)
    if (e.base != 0 && BlockExtentOn()) {
        std::unique_lock ext_lock{g_ext_mutex};
        g_ext.erase(e.base);
    }
    SotcPcPlaceGlue::Global().OnFreeObserved(e.base, e.size); // sotc: SOTC_PC_PLACE
    SotcPcRing::OnFreeBlock(e.base); // sotc: SOTC_PC_RING
    SotcRbClass::OnFreeBlock(e.base); // sotc: SOTC_RB_CLASS
    SotcN1Verify::OnFreeBlock(e.base); // sotc: SOTC_VERIFY=n1
    SotcNative::DiscardFreed(e.base, e.size); // sotc: SOTC_NATIVE_FREE_DISCARD
    PoolRow& row = g_pools[e.pool];
    row.live_bytes -= static_cast<int64_t>(e.size);
    if (row.live_bytes < 0) {
        row.live_bytes = 0;
    }
    row.w_frees += 1;
    row.w_free_bytes += e.size;
    if (e.owner != kNoOwner) {
        OwnerRow& o = g_owners[e.owner];
        o.live_bytes -= static_cast<int64_t>(e.size);
        if (o.live_bytes < 0) {
            o.live_bytes = 0;
        }
    }
    if (e.base) {
        g_freed[FreedSlot(e.base)] = e.base; // a later allocation at this base counts as VA reuse
    }
    return HookAction::Continue;
}

double Mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

const char* KindName(int32_t kind) {
    switch (kind) {
    case 1:
        return "CPU";
    case 2:
        return "GPU";
    default:
        return "?";
    }
}

} // namespace

void RegisterAlloc() {
    // Registered unconditionally: a hook that is off never dispatches to its handler.
    SotcHooks::Handler alloc;
    alloc.pre = &AllocPre;
    alloc.post = &AllocPost;
    SotcHooks::RegisterHandler("ALLOC_BLOCK", alloc);

    SotcHooks::Handler freed;
    freed.pre = &FreePre;
    SotcHooks::RegisterHandler("FREE_BLOCK", freed);
}

bool BlockExtentOn() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_BLOCK_EXTENT");
        return v && v[0] == '1';
    }();
    return on;
}

bool BlockRemaining(uint64_t addr, uint64_t* remaining) noexcept {
    std::shared_lock lock{g_ext_mutex};
    auto it = g_ext.upper_bound(addr);
    if (it == g_ext.begin()) {
        return false;
    }
    --it;
    if (addr - it->first >= it->second) {
        return false;
    }
    *remaining = it->second - (addr - it->first);
    return true;
}

// sotc: SOTC_VSIZE_PROBE. The live tracked block holding addr (linear scan of the node table, under the
// lock; the caller runs it once per buffer pair, not per bind).
bool FindBlock(uint64_t addr, BlockInfo* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const NodeEntry& e : g_nodes) {
        if (e.key == 0 || e.base == 0 || addr < e.base || addr - e.base >= e.size) {
            continue;
        }
        out->base = e.base;
        out->size = e.size;
        out->kind = g_pools[e.pool].kind;
        std::snprintf(out->pool, sizeof out->pool, "%s", g_pools[e.pool].name);
        return true;
    }
    return false;
}

void ReportAlloc(std::string& out, double seconds) {
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    // Snapshot the pools and owners and reset the window counters under the lock, then format without it.
    std::array<PoolRow, kMaxPools> rows{};
    std::array<OwnerRow, kMaxOwners> owners{};
    size_t count = 0;
    size_t owner_count = 0;
    uint64_t w_allocs = 0, w_reuse = 0, w_spills = 0, w_kind = 0, w_align = 0, w_owner_dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        count = g_pool_count;
        for (size_t i = 0; i < count; ++i) {
            rows[i] = g_pools[i];
            g_pools[i].w_allocs = 0;
            g_pools[i].w_frees = 0;
            g_pools[i].w_alloc_bytes = 0;
            g_pools[i].w_free_bytes = 0;
        }
        owner_count = g_owner_count;
        for (size_t i = 0; i < owner_count; ++i) {
            OwnerRow& o = g_owners[i];
            owners[i] = o;
            o.w_allocs = 0;
            o.w_alloc_bytes = 0;
            for (uint64_t& v : o.w_slot) {
                v = 0;
            }
            o.w_spills = 0;
        }
        w_allocs = g_w_allocs;
        w_reuse = g_w_reuse;
        w_spills = g_w_spills;
        w_kind = g_w_kind_mismatch;
        w_align = g_w_odd_align;
        w_owner_dropped = g_w_owner_dropped;
        g_w_allocs = g_w_reuse = g_w_spills = g_w_kind_mismatch = g_w_odd_align = g_w_owner_dropped = 0;
    }
    const uint64_t missed = g_missed_frees.exchange(0, std::memory_order_relaxed);
    const uint64_t dropped = g_dropped_allocs.exchange(0, std::memory_order_relaxed);
    const uint64_t overflow = g_overflow.exchange(0, std::memory_order_relaxed);

    bool moved = false;
    for (size_t i = 0; i < count; ++i) {
        if (rows[i].w_allocs || rows[i].w_frees) {
            moved = true;
            break;
        }
    }
    if (!moved && w_allocs == 0 && missed == 0 && dropped == 0 && overflow == 0) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        const PoolRow& r = rows[i];
        if (r.w_allocs == 0 && r.w_frees == 0) {
            continue; // one line per pool, only when something moved this window
        }
        char line[256];
        std::snprintf(line, sizeof line,
                      "SOTC ALLOC probe: %s (%s): +%llu allocs (%.2f MiB), -%llu frees (%.2f MiB) in "
                      "%.0f s; live %.2f MiB (peak %.2f MiB), %llu allocs total\n",
                      r.name[0] ? r.name : "?", KindName(r.kind),
                      static_cast<unsigned long long>(r.w_allocs), Mib(r.w_alloc_bytes),
                      static_cast<unsigned long long>(r.w_frees), Mib(r.w_free_bytes), seconds,
                      Mib(static_cast<uint64_t>(r.live_bytes)), Mib(r.peak_bytes),
                      static_cast<unsigned long long>(r.alloc_count));
        out += line;
    }

    // Top owners by bytes allocated in the window (at most 16 lines).
    std::vector<size_t> order;
    for (size_t i = 0; i < owner_count; ++i) {
        if (owners[i].w_allocs) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&owners](size_t a, size_t b) {
        return owners[a].w_alloc_bytes > owners[b].w_alloc_bytes;
    });
    const size_t shown = std::min<size_t>(order.size(), 16);
    for (size_t k = 0; k < shown; ++k) {
        const OwnerRow& o = owners[order[k]];
        const uint64_t live = o.live_bytes > 0 ? static_cast<uint64_t>(o.live_bytes) : 0;
        char line[256];
        std::snprintf(line, sizeof line,
                      "SOTC ALLOC OWNER 0x%llx %-20.20s k%d slot 0:%llu 1:%llu 2:%llu ?:%llu, "
                      "%.1f/s %.2f MiB/s live %.2f peak %.2f spills %llu\n",
                      static_cast<unsigned long long>(o.owner), o.name[0] ? o.name : "?", o.kind,
                      static_cast<unsigned long long>(o.w_slot[0]),
                      static_cast<unsigned long long>(o.w_slot[1]),
                      static_cast<unsigned long long>(o.w_slot[2]),
                      static_cast<unsigned long long>(o.w_slot[3]),
                      static_cast<double>(o.w_allocs) / seconds, Mib(o.w_alloc_bytes) / seconds,
                      Mib(live), Mib(o.peak_bytes), static_cast<unsigned long long>(o.w_spills));
        out += line;
    }
    if (w_allocs) {
        char va[256];
        std::snprintf(va, sizeof va,
                      "SOTC ALLOC VA: %llu of %llu allocs at a freed base; spills %llu; fwd kind != owner "
                      "kind %llu; align != 64 KiB %llu; owners untracked %llu in %.0f s\n",
                      static_cast<unsigned long long>(w_reuse),
                      static_cast<unsigned long long>(w_allocs),
                      static_cast<unsigned long long>(w_spills),
                      static_cast<unsigned long long>(w_kind),
                      static_cast<unsigned long long>(w_align),
                      static_cast<unsigned long long>(w_owner_dropped), seconds);
        out += va;
    }
    if (missed || dropped || overflow) {
        char note[192];
        std::snprintf(note, sizeof note,
                      "SOTC ALLOC probe: %llu untracked frees, %llu dropped allocs (table full), "
                      "%llu nesting overflows in %.0f s\n",
                      static_cast<unsigned long long>(missed),
                      static_cast<unsigned long long>(dropped),
                      static_cast<unsigned long long>(overflow), seconds);
        out += note;
    }
}

} // namespace SotcProbes
