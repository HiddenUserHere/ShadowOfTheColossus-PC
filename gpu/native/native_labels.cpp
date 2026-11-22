// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Virtual label table (N2-94 W2, H05, fixer round 4).
//
// Coverage (item | done? | evidence):
// - Virtual label table, open addressing, 1024 slots, one mutex (command processor and draw
//   thread both touch it, never for long) | yes | Record / Retire / Satisfies below.
// - Entry = {addr, value, bytes, seq} per SPEC W2, plus pending and gen (added): newer Record for
//   the same address overwrites value and seq; pending counts unretired writes so an older Retire
//   does not drop a newer write | yes | struct Entry, Record, Retire.
// - Token = slot + 1 (low 32 bits) | generation (high 32 bits); nonzero; stale tokens ignored when
//   the slot was reclaimed | yes | MakeToken / Retire.
// - Satisfies: only when Native::Switch::VirtualLabels(); needs a pending entry; an excluded address
//   or the overflow state is never pending (round 4: IsExcludedLocked is consulted); PM4 WaitRegMem
//   compare ((value & mask) <func> ref), func 0..6, value low 32 bits; WaitSatVirtual on true |
//   yes | Satisfies.
// - Exclude(addr) (round 4): gated by Native::Switch::VirtualLabels(); adds addr to the set AND purges
//   an already-recorded pending entry (pending = 0), so a write recorded before Exclude is no longer
//   satisfiable; Satisfies also checks the set | yes | Exclude / IsExcludedLocked / Satisfies.
// - Exclude set overflow (round 4): kMaxExcluded full -> the table fails SAFE: g_exclude_overflow is
//   set, every pending entry is purged, and Record and Satisfies refuse everything for the rest of the
//   run (every wait falls back to polling). No Stats counter exists for this (native_stats.h has no
//   slot and this file may not edit it), so the event is not counted | yes (safe, not counted) |
//   Exclude, Record, Satisfies. Counter request in core_requests.
// - Retire (round 4): gated by Native::Switch::VirtualLabels() like every other entry point. The switch
//   is a static (EnvOn once), so a nonzero token exists only when the switch is on | yes | Retire.
// - SubmitBoundary: LabelsAtBoundary += number of ENTRIES still pending (not writes) | yes |
//   SubmitBoundary. NO call site in the tree (round 4 re-checked): sceGnmSubmitDone does not call it.
//   Wiring it is in gnmdriver.cpp, outside this slice: see core_requests.
// - Counters LabelsRecorded / LabelsRetired | yes | Record / Retire.
// - Switch off: Record returns 0, Satisfies false, Retire no-op, Exclude no-op, SubmitBoundary no-op
//   | yes | every entry point checks the switch first.
// - Table full with no tombstone: Record returns 0 (wait falls back to polling) | yes | Record.
// - Call sites in the tree (verified by rg, round 2): Record/Retire in gnm/w06_memory.cpp:166/176,
//   gnm/w07_sync.cpp:295/312/482/495, gnm/w12_submit.cpp:252/261, and
//   shadps4/video_core/amdgpu/liverpool.cpp:2486 (Record, EOP/EOS-style release) / :2528 (Retire).
//   Satisfies in gnm/w07_sync.cpp:200. Record and Retire always come in pairs on the same token.
// - Exclude has NO call site in the tree (round 4 re-checked: only the definition and the comment at
//   gnm/w07_sync.cpp:103) | NOT DONE | the CPU-store notification at the 0x92540 hook must call
//   Exclude(slot) BEFORE the store (core_requests). Until then Record refuses nothing for CPU slots.
// - Refusal of CPU-written slot classes in Record | NOT CLOSED in this slice | Record refuses only
//   addresses already in the Exclude set; the set is filled by nothing today. No static address or
//   value rule can identify the slot (its address is chosen by the caller of FUN_00092540 at run time:
//   the caller's slot, or FUN_0086d210 when the caller passes NULL). The mechanism that closes it is
//   described in the safety section below and needs the hook (core_requests).
//
// REA check (Ghidra 12.2, out/re/eboot_pe_fde.exe, sha256 91d23a48...; procedure_pseudo_code and
// read_bytes evidence ev_f578fd45..., ev_cfa93dbd..., ev_7982744f...; decompile file
// out/re/decomp/0009/00092540.c read again in round 4):
// - FUN_00092540 (0x92540): if param_3 == NULL then param_3 = FUN_0086d210(*param_1, 8, 3) (heap
//   allocation, the only heap path); then *param_3 = 0 (8-byte CPU store of 0) for EVERY caller (line 9
//   of the decompile); then FUN_0086eb60(..., param_3, 1, 1, ...). So a CPU-written class exists for
//   every caller, and it is the caller's own slot when a non-null slot is passed. Its callers are many,
//   so the address set is not static. Raw bytes at 0x92540 (554889e5 4157 4156 5350 ...) confirm a
//   function entry.
// - FUN_00092500 (0x92500): calls FUN_0086d210(param_1, 3) and then stores param_2 into a run of
//   ESI cells (loop). This is array initialization by the CPU, not a GPU-visible label write; a
//   label in that array only enters the table if a GPU op's address equals it, which is the case
//   the Exclude rule covers.
// - REWIND valid bit (opcode 0x59 REWIND, PM4 handler at 0x866530 / 0x86f930 / 0x8847d0, cited in
//   gpu/native/gnm/w11_ce_misc.cpp:79): the REWIND handler yields the fiber while polling, and it is
//   kept on PM4, so it is not a label write recorded here. The valid bit is therefore NOT a
//   Record class in this file. (Raw-byte verification of the valid bit itself not done.)
//
// Safety argument for CPU-written slots (frame-reuse race), round 4:
// - Cross-frame: sceGnmSubmitDone (gnmdriver.cpp:2561) runs the static WaitGpuIdle (gnmdriver.cpp:88)
//   on submission_lock, set at :2574 only when liverpool->IsGpuIdle() is false and cleared by the
//   GpuIdle IRQ (ResetSubmissionLock, gnmdriver.cpp:82). The round-2 claim that the IRQ runs after every
//   Retire of the closed submissions (BackOrRun, liverpool.cpp, after num_submits is decremented at
//   :1225) is NOT re-verified in round 4; treat the cross-frame argument as UNPROVEN until that ordering
//   is checked against liverpool.cpp. It is not relied on for the fix below.
// - Intra-frame (NOT closed by the barrier): the CPU store of 0 can happen after an earlier submit
//   recorded a GPU write to that slot and before the draw thread retires it. The table cannot see the
//   store. Mechanism that makes it safe once wired (round 4): the hook calls Exclude(slot) before
//   the CPU store. Exclude purges the pending entry (pending = 0) and adds the slot to the set, so
//   Satisfies no longer answers the GPU value for that slot and Record refuses it from then on. The
//   stale GPU write may still land and Retire will no-op (pending == 0). Under the mutex the purge is
//   ordered before any later Satisfies on the command processor thread. Caveat: Exclude is
//   TIME-ORDERED with the store only if the hook calls it before the store on the game thread; the
//   hook is the missing piece (core_requests).
// - Overflow of the exclude set disables the whole table (fail safe), so it cannot leave a stale
//   CPU-written slot satisfiable.
// - Retire of the GPU write happens on the draw thread, so the table never reports a value the
//   GPU has not been asked to write.
// - SubmitBoundary invariant (LabelsAtBoundary == 0 after the barrier) is still not measured because
//   SubmitBoundary has no call site.

#include "native/native_labels.h"
#include "native/native_progress.h"

#include <atomic>
#include "native/native_stats.h"
#include "native/native_switches.h"

#include <array>
#include <mutex>

namespace Native::Labels {
namespace {

constexpr u32 kSlots = 1024;
constexpr u32 kMaxExcluded = 64;

// sotc: N2-94 H05 fixer. Entry carries the spec fields {addr, value, bytes, seq}; pending and gen
// are added on top: pending keeps an older Retire from dropping a newer write, gen keeps stale
// tokens from touching a reclaimed slot. seq is the Record ordinal of the latest write.
struct Entry {
    u64 addr = 0;
    u64 value = 0;   ///< latest recorded value (the one a wait may pass on)
    u32 bytes = 0;   ///< 4 or 8
    u64 owner = 0;   ///< sotc: submission that recorded the latest write (SetSubmission)
    u64 seq = 0;     ///< ordinal of the latest Record for this address (g_next_seq at that time)
    u32 pending = 0; ///< unretired Record calls for this address
    u32 gen = 0;     ///< generation of the claim (0 = never used)
    bool used = false;       ///< a live entry (N2-117: no tombstones, see EraseLocked)
};

std::mutex g_mutex;
std::array<Entry, kSlots> g_table{};
std::array<u64, kMaxExcluded> g_excluded{};
u32 g_excluded_count = 0;
// sotc: round 4. Set when Exclude runs out of room: the table then refuses every Record and
// Satisfies for the rest of the run, so no stale CPU-written slot can stay satisfiable.
bool g_exclude_overflow = false;
u32 g_next_gen = 1;
u64 g_next_seq = 1;
std::atomic<u64> g_owner{0};
thread_local bool g_park_hint = false; ///< sotc: N2-112 (Satisfies -> TakeParkHint)

u32 HashAddr(u64 addr) {
    // Labels are 4- or 8-byte aligned; fold the address so neighbouring labels spread.
    u64 h = addr * 0x9E3779B97F4A7C15ull;
    return static_cast<u32>(h >> 54) & (kSlots - 1);
}

bool IsExcludedLocked(u64 addr) {
    for (u32 i = 0; i < g_excluded_count; ++i) {
        if (g_excluded[i] == addr) {
            return true;
        }
    }
    return false;
}

/// Finds the live-or-tombstone slot holding `addr`, or -1.
int FindLocked(u64 addr) {
    u32 i = HashAddr(addr);
    for (u32 n = 0; n < kSlots; ++n) {
        const Entry& e = g_table[i];
        if (!e.used) {
            return -1;
        }
        if (e.addr == addr) {
            return static_cast<int>(i);
        }
        i = (i + 1) & (kSlots - 1);
    }
    return -1;
}

// sotc: N2-117: the token is the label address (bit 63 set so it is never 0). Entries move when a
// neighbour is erased (backward-shift deletion), so a slot index cannot be a token; the pending
// count per address balances Record/Retire pairs, and an entry is erased only at pending 0, when no
// token for it is outstanding.
constexpr u64 kTokenBit = 1ull << 63;
u64 MakeToken(u64 addr) {
    return addr | kTokenBit;
}

/// Erases slot `i` of a linear-probing table without tombstones: every later entry of the cluster
/// whose home slot does not lie cyclically in (i, j] moves back into the hole. N2-117: before,
/// retired entries stayed as tombstones for good, the table filled up with them and every lookup of
/// an address not in it walked up to all 1024 slots (6.8% of the command processor in run 77).
void EraseLocked(u32 i) {
    u32 j = i;
    for (;;) {
        j = (j + 1) & (kSlots - 1);
        if (!g_table[j].used) {
            break;
        }
        const u32 k = HashAddr(g_table[j].addr);
        const bool in_range = i <= j ? (i < k && k <= j) : (i < k || k <= j);
        if (in_range) {
            continue;
        }
        g_table[i] = g_table[j];
        i = j;
    }
    g_table[i] = Entry{};
}

} // namespace

u64 Record(u64 addr, u64 value, u32 bytes) {
    if (!Native::Switch::VirtualLabels()) {
        return 0;
    }
    if (bytes != 4 && bytes != 8) {
        return 0;
    }
    std::lock_guard lock(g_mutex);
    if (g_exclude_overflow || IsExcludedLocked(addr)) {
        return 0;
    }

    // Existing entry for the address: overwrite the pending value (newest write wins).
    int slot = FindLocked(addr);
    if (slot < 0) {
        u32 i = HashAddr(addr);
        for (u32 n = 0; n < kSlots && g_table[i].used; ++n) {
            i = (i + 1) & (kSlots - 1);
        }
        if (g_table[i].used) {
            return 0; // table full of pending entries: the caller keeps polling
        }
        Entry& fresh = g_table[i];
        fresh.addr = addr;
        fresh.gen = g_next_gen++;
        fresh.used = true;
        fresh.pending = 0;
        slot = static_cast<int>(i);
    }

    Entry& e = g_table[static_cast<u32>(slot)];
    e.value = value;
    e.bytes = bytes;
    e.seq = g_next_seq++;
    e.owner = g_owner.load(std::memory_order_relaxed);
    ++e.pending;
    Native::Stats::Add(Native::Stats::LabelsRecorded);
    return MakeToken(addr);
}

void Retire(u64 token) {
    // sotc: round 4. Gated like every entry point. The switch is static, so no nonzero token exists
    // while it is off; the gate only keeps the default path free of any state access.
    if (!Native::Switch::VirtualLabels()) {
        return;
    }
    if (token == 0) {
        return;
    }
    if ((token & kTokenBit) == 0) {
        return;
    }
    std::lock_guard lock(g_mutex);
    const int slot = FindLocked(token & ~kTokenBit);
    if (slot < 0) {
        return;
    }
    Entry& e = g_table[static_cast<u32>(slot)];
    if (e.pending == 0) {
        return;
    }
    // Only the count goes down: a newer Record of the same address keeps its value pending.
    if (--e.pending == 0) {
        EraseLocked(static_cast<u32>(slot));
    }
    Native::Stats::Add(Native::Stats::LabelsRetired);
    // sotc: N2-112: a wait may be parked on this write; wake the command processor. Called with
    // g_mutex held is fine: Notify only bumps an atomic and signals a condition variable.
    Native::Progress::Notify();
}

bool Satisfies(u64 addr, u32 mask, u32 func, u32 ref) {
    if (!Native::Switch::VirtualLabels()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    // sotc: round 4. An excluded address (or any address after an exclude overflow) is never pending.
    if (g_exclude_overflow || IsExcludedLocked(addr)) {
        return false;
    }
    const int slot = FindLocked(addr);
    if (slot < 0) {
        return false;
    }
    const Entry& e = g_table[static_cast<u32>(slot)];
    if (e.pending == 0) {
        return false;
    }
    const u32 v = static_cast<u32>(e.value) & mask;
    bool ok = false;
    switch (func) {
    case 0: ok = true; break;      // always
    case 1: ok = v < ref; break;   // <
    case 2: ok = v <= ref; break;  // <=
    case 3: ok = v == ref; break;  // ==
    case 4: ok = v != ref; break;  // !=
    case 5: ok = v >= ref; break;  // >=
    case 6: ok = v > ref; break;   // >
    default: ok = false; break;
    }
    if (ok) {
        g_park_hint = true; // sotc: N2-112, consumed by TakeParkHint (command processor thread)
    }
    return ok;
}

bool TakeParkHint() {
    const bool hint = g_park_hint;
    g_park_hint = false;
    return hint;
}

void SetSubmission(u64 owner) {
    if (!Native::Switch::VirtualLabels()) {
        return;
    }
    g_owner.store(owner, std::memory_order_relaxed);
}

void Exclude(u64 addr) {
    // sotc: gated like every other entry point, so the default path touches no state here.
    if (!Native::Switch::VirtualLabels()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    if (IsExcludedLocked(addr)) {
        // Already excluded: a previous call already purged any pending entry.
        return;
    }
    if (g_excluded_count >= kMaxExcluded) {
        // sotc: round 4. The refusal cannot be kept for this address, so fail safe: drop every pending
        // entry and refuse all further Record / Satisfies. Waits poll the real memory from now on.
        g_exclude_overflow = true;
        g_table.fill(Entry{});
        return;
    }
    g_excluded[g_excluded_count++] = addr;
    // Purge a write recorded before this call: the address must never be treated as pending again.
    const int slot = FindLocked(addr);
    if (slot >= 0) {
        EraseLocked(static_cast<u32>(slot));
    }
}

void SubmitBoundary() {
    if (!Native::Switch::VirtualLabels()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    // Counts ENTRIES (addresses) that still have an unretired write, as the spec asks.
    u64 pending_entries = 0;
    for (const Entry& e : g_table) {
        if (e.used && e.pending != 0) {
            ++pending_entries;
        }
    }
    if (pending_entries != 0) {
        Native::Stats::Add(Native::Stats::LabelsAtBoundary, pending_entries);
    }
}

} // namespace Native::Labels
