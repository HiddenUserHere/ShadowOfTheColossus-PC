// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. See native_stream_ops.h.

#include "native/native_stream_ops.h"

#include "native/native_stats.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace Native::StreamOps {

namespace {

/// PM4CmdNop::PayloadType::SotcNativeOp (pm4_cmds.h); repeated here so this module does not pull
/// the PM4 headers.
constexpr u32 kPayload = 0x68750f01u;
/// PM4CmdNop::PayloadType::SotcNativeWait.
constexpr u32 kWaitPayload = 0x68750f02u;
/// PM4CmdNop::PayloadType::SotcNativeWaitCe.
constexpr u32 kWaitCePayload = 0x68750f03u;
/// PM4CmdNop::PayloadType::SotcNativeIb.
constexpr u32 kIbPayload = 0x68750f04u;
/// Type-3 NOP header: bits 30-31 = 3, count (body dwords - 1) in bits 16-29, opcode NOP (0x10) in
/// bits 8-15. Same encoding the guest's 0x86d210 uses for its label NOPs.
constexpr u32 NopHeader(u32 body_dwords) {
    return 0xc0001000u | ((body_dwords - 1) << 16);
}
/// Ops never reached by the command processor (a command buffer the game drops) would otherwise
/// accumulate; past this many pending ones the oldest ids are simply never run again.
/// sotc: N2-94 W4, round 3. Default (SOTC_UF_SBO=0): UniqueFunction is one pointer, a slot is 16 bytes
/// and the ring keeps the old 2^19 entries (8 MB per ring), so the default build is unchanged. With
/// SOTC_UF_SBO=1 the inline wrapper makes a slot larger (roughly 96 bytes), so the ring is cut to 2^17
/// entries to keep its footprint near the default one.
constexpr std::size_t kMaxPending = Common::kUniqueFunctionSbo ? (1u << 17) : (1u << 19);

/// Pending ops and waits: a ring of slots indexed by the low bits of the (sequential) op id, each
/// with its own tiny lock (gap N2-89). It replaced one global mutex + unordered_map, which every
/// guest thread emitting an op and the command processor running one contended on (~0.2-0.5 M
/// ops/s) and which allocated a node per op. A slot still holding an op kRing ids later belongs to
/// a command buffer the game dropped (or one built very far ahead): the new op is refused and its
/// caller keeps the guest path, as with the old map when full.
///
/// sotc: N2-94 W4. Fn is a Common::UniqueFunction: with SOTC_UF_SBO=1 it has 64-byte inline storage
/// (slot roughly 96 bytes); with the default SOTC_UF_SBO=0 it is one pointer, so the slot is 16 bytes
/// as before. The ring is default-constructed at start-up, so its pages are touched then. Moves
/// (Put/Take/Poll) go through UniqueFunction's move operation, which moves an inline callable and
/// leaves the source empty; Take and Poll move the callable out of the slot, so the slot's Fn is
/// empty afterwards and no callable is destroyed twice. Take moves the callable into the caller's
/// local (`out`), so the captured state is released when that local goes out of scope, not inside
/// Take itself.
template <typename Fn>
struct Slot {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    u32 tag = 0; ///< id without flag bits; 0 = empty
    Fn fn;
    void Lock() {
        while (busy.test_and_set(std::memory_order_acquire)) {
            while (busy.test(std::memory_order_relaxed)) {
            }
        }
    }
    void Unlock() {
        busy.clear(std::memory_order_release);
    }
};
constexpr u32 kRing = static_cast<u32>(kMaxPending);
static_assert((kRing & (kRing - 1)) == 0, "ring size must be a power of two");
Slot<Common::UniqueFunction<void>>* g_ops = new Slot<Common::UniqueFunction<void>>[kRing];
Slot<Common::UniqueFunction<bool>>* g_waits = new Slot<Common::UniqueFunction<bool>>[kRing];
std::atomic<u32> g_next_id{1};
std::atomic<u32> g_missing{0};

template <typename Fn>
bool Put(Slot<Fn>* ring, u32 id, Fn&& fn) {
    const u32 key = id & ~kIdFlagMask;
    Slot<Fn>& s = ring[key & (kRing - 1)];
    s.Lock();
    if (s.tag != 0) {
        // Still pending kRing ids later: refuse, so the caller keeps the guest PM4 path (as the
        // old map did when full) instead of losing an op that may still run.
        s.Unlock();
        return false;
    }
    s.fn = std::move(fn);
    s.tag = key;
    s.Unlock();
    return true;
}

template <typename Fn>
bool Take(Slot<Fn>* ring, u32 id, Fn& out) {
    const u32 key = id & ~kIdFlagMask;
    Slot<Fn>& s = ring[key & (kRing - 1)];
    s.Lock();
    const bool hit = s.tag == key;
    if (hit) {
        out = std::move(s.fn);
        s.tag = 0;
    }
    s.Unlock();
    return hit;
}

/// The guest's refill callback (`(**(code **)(cb + 0x18))(cb, dwords, *(cb + 0x20))`, returns a
/// bool in al), guest SysV.
using RefillFn = u8(PS4_SYSV_ABI*)(u64 cb, u32 dwords, u64 arg);

/// Reserves `count` dwords at the guest command buffer's write cursor (refilling through the
/// guest callback when short) and returns the start, or nullptr. Does not advance the cursor.
u32* Reserve(u64 cb, u32 count) {
    if (cb == 0) {
        return nullptr;
    }
    auto** cursor_slot = reinterpret_cast<u32**>(cb + 0x10);
    u32* cursor = *cursor_slot;
    const u32* end = *reinterpret_cast<u32* const*>(cb + 0x08);
    if (cursor == nullptr || end < cursor || static_cast<u64>(end - cursor) < count) {
        const auto refill = *reinterpret_cast<const RefillFn*>(cb + 0x18);
        if (refill == nullptr || refill(cb, count, *reinterpret_cast<const u64*>(cb + 0x20)) == 0) {
            return nullptr;
        }
        cursor = *cursor_slot;
        end = *reinterpret_cast<u32* const*>(cb + 0x08);
        if (cursor == nullptr || end < cursor || static_cast<u64>(end - cursor) < count) {
            return nullptr;
        }
    }
    return cursor;
}

void Advance(u64 cb, u32* start, u32 count) {
    *reinterpret_cast<u32**>(cb + 0x10) = start + count;
}

u32 NextId(bool writes_memory, Where where) {
    u32 id = g_next_id.fetch_add(1, std::memory_order_relaxed) & ~kIdFlagMask;
    if (id == 0) { // wrapped: 0 is never handed out
        id = g_next_id.fetch_add(1, std::memory_order_relaxed) & ~kIdFlagMask;
    }
    if (writes_memory) {
        id |= kWritesMemoryBit;
    }
    if (where == Where::CommandProcessor) {
        id |= kRunOnCpBit;
    }
    return id;
}

bool Queue(u32 id, Common::UniqueFunction<void>&& op) {
    return Put(g_ops, id, std::move(op));
}

void Unqueue(u32 id) {
    Common::UniqueFunction<void> drop;
    Take(g_ops, id, drop);
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("SOTC_NATIVE_STREAM_OPS");
        return !e || e[0] != '0';
    }();
    return enabled;
}

bool Emit(u64 cb, Common::UniqueFunction<void>&& op, bool writes_memory, Where where) {
    return EmitWithSpace(cb, std::move(op), 0, 4, writes_memory, where) != 0;
}

u64 EmitWithSpace(u64 cb, Common::UniqueFunction<void>&& op, u32 body_dwords, u32 align_bytes,
                  bool writes_memory, Where where) {
    const u32 id = NextId(writes_memory, where);
    if (!Queue(id, std::move(op))) {
        Stats::Add(Stats::OpsEmitFailed);
        return 0;
    }
    if (align_bytes < 4) {
        align_bytes = 4;
    }
    // Worst case padding is align/4 - 1 dwords; reserve for it, then lay out exactly.
    const u32 max_pad = body_dwords != 0 ? align_bytes / 4 - 1 : 0;
    u32* start = Reserve(cb, 3 + max_pad + body_dwords);
    if (start == nullptr) {
        Unqueue(id);
        Stats::Add(Stats::OpsEmitFailed);
        return 0;
    }
    u32 pad = 0;
    if (body_dwords != 0) {
        const u64 body = reinterpret_cast<u64>(start + 3);
        const u64 aligned = (body + align_bytes - 1) & ~static_cast<u64>(align_bytes - 1);
        pad = static_cast<u32>((aligned - body) / 4);
    }
    const u32 total = 3 + pad + body_dwords;
    start[0] = NopHeader(total - 1);
    start[1] = kPayload;
    start[2] = id;
    for (u32 i = 3; i < 3 + pad; ++i) {
        start[i] = 0;
    }
    Advance(cb, start, total);
    Stats::Add(Stats::OpsEmitted);
    return reinterpret_cast<u64>(start + 3 + pad);
}

bool EmitInSlot(u32* slot, u32 size_dwords, Common::UniqueFunction<void>&& op,
                bool writes_memory, Where where) {
    if (slot == nullptr || size_dwords < 3) {
        Stats::Add(Stats::OpsEmitFailed);
        return false;
    }
    const u32 id = NextId(writes_memory, where);
    if (!Queue(id, std::move(op))) {
        Stats::Add(Stats::OpsEmitFailed);
        return false;
    }
    slot[0] = NopHeader(size_dwords - 1);
    slot[1] = kPayload;
    slot[2] = id;
    for (u32 i = 3; i < size_dwords; ++i) {
        slot[i] = 0;
    }
    Stats::Add(Stats::OpsEmitted);
    return true;
}

namespace {
u32 QueueWait(Common::UniqueFunction<bool>&& predicate) {
    const u32 id = NextId(false, Where::CommandProcessor);
    return Put(g_waits, id, std::move(predicate)) ? id : 0;
}

void UnqueueWait(u32 id) {
    Common::UniqueFunction<bool> drop;
    Take(g_waits, id, drop);
}

bool EmitWaitPayload(u64 cb, Common::UniqueFunction<bool>&& predicate, u32 payload) {
    const u32 id = QueueWait(std::move(predicate));
    if (id == 0) {
        Stats::Add(Stats::OpsEmitFailed);
        return false;
    }
    u32* start = Reserve(cb, 3);
    if (start == nullptr) {
        UnqueueWait(id);
        Stats::Add(Stats::OpsEmitFailed);
        return false;
    }
    start[0] = NopHeader(2);
    start[1] = payload;
    start[2] = id;
    Advance(cb, start, 3);
    Stats::Add(Stats::OpsEmitted);
    return true;
}
} // namespace

bool EmitIb(u64 cb, u64 address, u32 size_dwords) {
    // sotc: exactly the 4 dwords of the guest's IT_INDIRECT_BUFFER (N2-108). The compute ring is
    // filled with 4-dword IB packets up to its end; a 5-dword NOP left an unpadded tail of 1-4
    // dwords at the ring end when the guest refill wrapped, which the parser read as PM4 type 0
    // (attract demo, ~28 s). Layout: payload, address bits 0-31, address bits 32-39 | size << 8.
    if ((address >> 40) != 0 || size_dwords >= (1u << 24)) {
        return false; // not representable: the caller keeps the guest packet
    }
    u32* start = Reserve(cb, 4);
    if (start == nullptr) {
        Stats::Add(Stats::OpsEmitFailed);
        return false;
    }
    start[0] = NopHeader(3);
    start[1] = kIbPayload;
    start[2] = static_cast<u32>(address);
    start[3] = static_cast<u32>(address >> 32) | (size_dwords << 8);
    Advance(cb, start, 4);
    Stats::Add(Stats::OpsEmitted);
    Stats::Add(Stats::OpsRun); // consumed by the parser itself, not by Run()
    return true;
}

bool EmitWaitCe(u64 cb, Common::UniqueFunction<bool>&& predicate) {
    return EmitWaitPayload(cb, std::move(predicate), kWaitCePayload);
}

bool EmitWaitInSlot(u32* slot, u32 size_dwords, Common::UniqueFunction<bool>&& predicate) {
    if (slot == nullptr || size_dwords < 3) {
        Stats::Add(Stats::OpsEmitFailed);
        return false;
    }
    const u32 id = QueueWait(std::move(predicate));
    if (id == 0) {
        Stats::Add(Stats::OpsEmitFailed);
        return false;
    }
    slot[0] = NopHeader(size_dwords - 1);
    slot[1] = kWaitPayload;
    slot[2] = id;
    for (u32 i = 3; i < size_dwords; ++i) {
        slot[i] = 0;
    }
    Stats::Add(Stats::OpsEmitted);
    return true;
}

bool EmitWait(u64 cb, Common::UniqueFunction<bool>&& predicate) {
    return EmitWaitPayload(cb, std::move(predicate), kWaitPayload);
}

bool Poll(u32 id_dword) {
    const u32 key = id_dword & ~kIdFlagMask;
    auto& s = g_waits[key & (kRing - 1)];
    // Only the command processor polls and clears; the predicate runs under the slot lock, which
    // no other thread touches until kRing more waits were queued.
    s.Lock();
    if (s.tag != key) {
        s.Unlock();
        Stats::Add(Stats::OpsMissing);
        return true;
    }
    if (!s.fn()) {
        s.Unlock();
        return false;
    }
    Common::UniqueFunction<bool> done = std::move(s.fn);
    s.tag = 0;
    s.Unlock();
    Stats::Add(Stats::OpsRun);
    return true;
}

void Run(u32 id_dword) {
    Common::UniqueFunction<void> op;
    if (!Take(g_ops, id_dword, op)) {
        Stats::Add(Stats::OpsMissing);
        if (g_missing.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::fprintf(stderr, "SOTC RENDER native: stream op %u not queued (skipped)\n",
                         id_dword & ~kIdFlagMask);
        }
        return;
    }
    op();
    Stats::Add(Stats::OpsRun);
}

} // namespace Native::StreamOps
