// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: XPR memory probe (PC memory model wave B, docs/re-stutters/PC-MEMORY-REWRITE.md sections 4
// and 5, items 1 and 3). Hooks (scripts/hooks.d/xprmem.py), switch SOTC_HOOK_XPRMEM_PROBE:
//
//   XPRMEM_BUILD_MASKS       wrap at BPXPR_BuildSetDraws (0x19b760). Pre: rdi = the mask owner (+0x18
//                            non-zero when the call writes masks, u32 +0x148 = entry index), rsi = the
//                            set (+0x38 = table of 0xa8-byte entries). The function writes its four
//                            GPU mask pointers at table + index * 0xa8 + {0, 8, 0x10, 0x18} just before
//                            it returns (decompiled 0x19b760, the puVar7 stores). Post: reads them.
//                            The masks come from 0x12820 with the memtype slot global at VA 0x2ed1f58.
//   XPRMEM_PRERENDER_BLOCKS  wrap at sub_ca150 (0xca150), called by XPRPreRender (0xca6d0) with
//                            rdi = the frame object. It allocates the per-frame table (0xae40 bytes)
//                            and stores it at frame+0x38. The four GPU block pointers are read by the
//                            callee from the global object at VA 0x2ed33f0 (not from the frame object):
//                            +0xa80, +0xa88, +0xa90, +0xa98. Post: reads the global, the four pointers
//                            and the table.
//
// Observe only: every handler returns HookAction::Continue and nothing in the guest is written.
// Guest reads go through the backing view (runtime_memory_backing_pointer, as probe_xpr.cpp does),
// bounded to the mapped region. Globals inside the loaded image are read from the image directly
// (the image is not in the backing view), as probe_alloc.cpp does.
//
// Concurrency: the counters are lock-free atomics. The per-owner table (repeat check) and the
// distinct-page table are the only shared state with a lock or CAS; the owner table is guarded by a
// mutex (BuildSetDraws runs a few dozen times a frame, not per draw). Window resets race with hooks
// that are still running: a count can land in the next window. That is accepted for a probe.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "probes.h"
#include "rb_host.h"                // sotc: SOTC_RB_HOST (mask pointers host-served count)
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

// Owner (BuildSetDraws argument rdi) and set (rsi) layout, from the decompiled 0x19b760.
constexpr uint64_t kOwnerGateOffset = 0x18;   // u64: non-zero when the call writes the masks
constexpr uint64_t kOwnerIndexOffset = 0x148; // u32: entry index in the set table
constexpr uint64_t kSetTableOffset = 0x38;    // u64 (set+0x38): table of 0xa8-byte entries
constexpr uint64_t kEntrySize = 0xa8;
constexpr uint64_t kMaskOffsets[4] = {0x0, 0x8, 0x10, 0x18};

constexpr uint64_t kSlotGlobal = 0x2ed1f58;   // DAT_02ed1f58: memtype slot of the mask family
constexpr uint64_t kCtxGlobal = 0x2ed33f0;    // DAT_02ed33f0: object holding the four GPU blocks
constexpr uint64_t kBlockOffsets[4] = {0xa80, 0xa88, 0xa90, 0xa98};
constexpr uint64_t kFrameTableOffset = 0x38;  // frame object +0x38: the per-frame table (0xae40 bytes)

constexpr size_t kPageSlots = 4096;           // distinct 64 KiB pages per window (power of two)
constexpr unsigned kPageBits = 12;
constexpr size_t kOwnerSlots = 256;           // distinct mask owners remembered (repeat check)
constexpr int kSamples = 4;                   // first mask addresses of the window

// Guest memory through the backing view, bounded to the mapped region. False when unmapped, outside
// the region, or not in the backing view.
bool ReadGuest(uintptr_t address, void* out, size_t n) {
    if (!address) {
        return false;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || n > end - address) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

// A global at image offset va (the eboot VA the decompiler prints), read directly from the image.
bool ReadImage(uint64_t va, void* out, size_t n) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (base == 0 || size == 0 || va >= size || n > size - va) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(base + va)), n);
    return true;
}

// ---- Mask window state -------------------------------------------------------------------------

// One mask owner (BuildSetDraws entry address) and the four masks it wrote last.
struct OwnerRow {
    uintptr_t entry = 0; // table + index * 0xa8 (0 = unused row)
    uint64_t last[4] = {0, 0, 0, 0};
};
std::mutex g_owner_mutex;
std::array<OwnerRow, kOwnerSlots> g_owners{};
size_t g_owner_count = 0;

std::array<std::atomic<uint64_t>, kPageSlots> g_pages{}; // page + 1 per slot, 0 = empty

std::atomic<uint64_t> g_mask_calls{0};        // calls that wrote masks (gate non-zero)
std::atomic<uint64_t> g_masks{0};             // mask pointers seen (4 per call)
std::atomic<uint64_t> g_compared{0};          // masks with a previous value for the same owner
std::atomic<uint64_t> g_repeats{0};           // ... of which equal to that previous value
std::atomic<uint64_t> g_distinct_pages{0};    // distinct 64 KiB pages this window
std::atomic<uint64_t> g_page_overflow{0};     // page table full (pages not counted)
std::atomic<uint64_t> g_owner_overflow{0};    // owner table full (repeat not checked)
std::atomic<uint64_t> g_mask_unreadable{0};   // owner, set or entry not readable
std::atomic<uint64_t> g_sample_seq{0};        // next sample index (first kSamples masks)
std::atomic<uintptr_t> g_samples[kSamples] = {};

// Inserts a 64 KiB page (guest address >> 16) into the window's page table.
void NotePage(uint64_t page) {
    const uint64_t key = page + 1;
    const size_t home = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> (64 - kPageBits));
    for (size_t probe = 0; probe < kPageSlots; ++probe) {
        std::atomic<uint64_t>& slot = g_pages[(home + probe) & (kPageSlots - 1)];
        uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == key) {
            return;
        }
        if (cur == 0) {
            uint64_t expected = 0;
            if (slot.compare_exchange_strong(expected, key, std::memory_order_relaxed)) {
                g_distinct_pages.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (expected == key) {
                return;
            }
        }
    }
    g_page_overflow.fetch_add(1, std::memory_order_relaxed);
}

// Reads the four mask pointers in the owner's current set-table entry.
void ReadMasks(uintptr_t owner, uintptr_t set);

// BPXPR_BuildSetDraws entry (0x19b760). rdi = owner, rsi = set. Entry, not wrap: the function
// realigns its stack (and rsp,-32), which the wrap detour refuses (first live run, 1 Nov). At entry
// the owner's current entry still holds the masks of its previous build (the frame before), which
// is enough for where they live and whether they repeat.
HookAction MaskPre(GuestRegs& regs, void*) {
    ReadMasks(static_cast<uintptr_t>(regs.rdi), static_cast<uintptr_t>(regs.rsi));
    return HookAction::Continue;
}

void ReadMasks(uintptr_t owner, uintptr_t set) {

    uint64_t gate = 0;
    if (!ReadGuest(owner + kOwnerGateOffset, &gate, sizeof gate)) {
        g_mask_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (gate == 0) {
        return; // the function returned early: no masks were written
    }

    uint32_t index = 0;
    uint64_t table = 0;
    if (!ReadGuest(owner + kOwnerIndexOffset, &index, sizeof index) ||
        !ReadGuest(set + kSetTableOffset, &table, sizeof table)) {
        g_mask_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uintptr_t entry = static_cast<uintptr_t>(table + static_cast<uint64_t>(index) * kEntrySize);
    uint64_t masks[4] = {0, 0, 0, 0};
    for (size_t j = 0; j < 4; ++j) {
        if (!ReadGuest(entry + kMaskOffsets[j], &masks[j], sizeof masks[j])) {
            g_mask_unreadable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    g_mask_calls.fetch_add(1, std::memory_order_relaxed);
    // sotc: SOTC_RB_HOST (R1): are the masks this owner wrote last inside a host-served run? Bytes as in
    // probe_xpr.cpp RecordMask: 4 * ceil(n / 32), n = u16 at set+0x64. Counted only; nothing is served here.
    if (SotcRbHost::On()) {
        uint16_t count = 0;
        if (ReadGuest(set + 0x64, &count, sizeof count) && count != 0) {
            const uint64_t bytes = 4ull * ((static_cast<uint64_t>(count) + 31) / 32);
            for (size_t j = 0; j < 4; ++j) {
                if (masks[j] != 0) {
                    SotcRbHost::NoteMaskPointer(masks[j], bytes);
                }
            }
        }
    }
    for (size_t j = 0; j < 4; ++j) {
        const uint64_t m = masks[j];
        if (m == 0) {
            continue;
        }
        g_masks.fetch_add(1, std::memory_order_relaxed);
        NotePage(m >> 16);
        const uint64_t seq = g_sample_seq.fetch_add(1, std::memory_order_relaxed);
        if (seq < kSamples) {
            g_samples[seq].store(static_cast<uintptr_t>(m), std::memory_order_relaxed);
        }
    }

    // Repeat check: does this owner's entry hold the same masks as at its previous write?
    std::lock_guard<std::mutex> lock(g_owner_mutex);
    OwnerRow* row = nullptr;
    for (size_t i = 0; i < g_owner_count; ++i) {
        if (g_owners[i].entry == entry) {
            row = &g_owners[i];
            break;
        }
    }
    if (!row) {
        if (g_owner_count == kOwnerSlots) {
            g_owner_overflow.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        row = &g_owners[g_owner_count++];
        row->entry = entry;
        for (size_t j = 0; j < 4; ++j) {
            row->last[j] = masks[j];
        }
        return; // first write of this owner: nothing to compare
    }
    for (size_t j = 0; j < 4; ++j) {
        if (masks[j] == 0) {
            continue;
        }
        g_compared.fetch_add(1, std::memory_order_relaxed);
        if (row->last[j] == masks[j]) {
            g_repeats.fetch_add(1, std::memory_order_relaxed);
        }
        row->last[j] = masks[j];
    }
}

// ---- Block window state ------------------------------------------------------------------------

thread_local uintptr_t t_frame = 0; // rdi at the sub_ca150 entry, read again in its post phase

std::atomic<uint64_t> g_block_calls{0};
std::atomic<uint64_t> g_block_unreadable{0};
std::atomic<uint64_t> g_block_changes[4] = {};   // calls where the pointer differs from the last call
std::atomic<uint64_t> g_block_last[4] = {};      // the latest pointer values (not reset)
std::atomic<uint64_t> g_ctx_last{0};             // the latest global object (DAT_02ed33f0)
std::atomic<uint64_t> g_ctx_is_frame{0};         // calls where the global object equals rdi
std::atomic<uint64_t> g_table_min{UINT64_MAX};   // per-frame table address range this window
std::atomic<uint64_t> g_table_max{0};
std::atomic<uint64_t> g_table_unreadable{0};

void AtomicMin(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void AtomicMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

// sub_ca150 entry: rdi = the frame object.
HookAction BlocksPre(GuestRegs& regs, void*) {
    t_frame = static_cast<uintptr_t>(regs.rdi);
    return HookAction::Continue;
}

// sub_ca150 return: the four GPU block pointers (from the global object) and the table it allocated.
void BlocksPost(GuestRegs&, void*) {
    const uintptr_t frame = t_frame;
    t_frame = 0;
    g_block_calls.fetch_add(1, std::memory_order_relaxed);

    uint64_t ctx = 0;
    if (!ReadImage(kCtxGlobal, &ctx, sizeof ctx) || ctx == 0) {
        g_block_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ctx_last.store(ctx, std::memory_order_relaxed);
    if (ctx == frame) {
        g_ctx_is_frame.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t values[4] = {0, 0, 0, 0};
    bool ok = true;
    for (size_t k = 0; k < 4 && ok; ++k) {
        ok = ReadGuest(static_cast<uintptr_t>(ctx + kBlockOffsets[k]), &values[k], sizeof values[k]);
    }
    if (!ok) {
        g_block_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (size_t k = 0; k < 4; ++k) {
        const uint64_t prev = g_block_last[k].exchange(values[k], std::memory_order_relaxed);
        if (prev != values[k]) {
            g_block_changes[k].fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t table = 0;
    if (frame != 0 && ReadGuest(frame + kFrameTableOffset, &table, sizeof table) && table != 0) {
        AtomicMin(g_table_min, table);
        AtomicMax(g_table_max, table);
    } else {
        g_table_unreadable.fetch_add(1, std::memory_order_relaxed);
    }
}

// "0x..." text of a guest address or value.
std::string Hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

void RegisterXprMem() {
    // Registered unconditionally: HookMode() is only known after InstallHooks, and a hook that is
    // off never dispatches to its handler.
    SotcHooks::Handler masks;
    masks.pre = &MaskPre;
    SotcHooks::RegisterHandler("XPRMEM_BUILD_MASKS", masks);

    SotcHooks::Handler blocks;
    blocks.pre = &BlocksPre;
    blocks.post = &BlocksPost;
    SotcHooks::RegisterHandler("XPRMEM_PRERENDER_BLOCKS", blocks);
}

void ReportXprMem(std::string& out, double seconds) {
    const uint64_t mask_calls = g_mask_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t masks = g_masks.exchange(0, std::memory_order_relaxed);
    const uint64_t compared = g_compared.exchange(0, std::memory_order_relaxed);
    const uint64_t repeats = g_repeats.exchange(0, std::memory_order_relaxed);
    const uint64_t distinct = g_distinct_pages.exchange(0, std::memory_order_relaxed);
    const uint64_t page_overflow = g_page_overflow.exchange(0, std::memory_order_relaxed);
    const uint64_t owner_overflow = g_owner_overflow.exchange(0, std::memory_order_relaxed);
    const uint64_t mask_unreadable = g_mask_unreadable.exchange(0, std::memory_order_relaxed);
    g_sample_seq.store(0, std::memory_order_relaxed);
    uintptr_t samples[kSamples];
    for (int i = 0; i < kSamples; ++i) {
        samples[i] = g_samples[i].exchange(0, std::memory_order_relaxed);
    }
    for (auto& slot : g_pages) {
        slot.store(0, std::memory_order_relaxed);
    }

    const uint64_t block_calls = g_block_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t block_unreadable = g_block_unreadable.exchange(0, std::memory_order_relaxed);
    uint64_t changes[4];
    for (size_t k = 0; k < 4; ++k) {
        changes[k] = g_block_changes[k].exchange(0, std::memory_order_relaxed);
    }
    const uint64_t table_min = g_table_min.exchange(UINT64_MAX, std::memory_order_relaxed);
    const uint64_t table_max = g_table_max.exchange(0, std::memory_order_relaxed);
    const uint64_t table_unreadable = g_table_unreadable.exchange(0, std::memory_order_relaxed);
    const uint64_t ctx_is_frame = g_ctx_is_frame.exchange(0, std::memory_order_relaxed);

    const double window = seconds > 0 ? seconds : 1.0;

    if (mask_calls != 0 || mask_unreadable != 0) {
        uint64_t slot = 0;
        const bool slot_ok = ReadImage(kSlotGlobal, &slot, sizeof slot);
        std::string sample_text;
        for (int i = 0; i < kSamples; ++i) {
            if (samples[i] != 0) {
                sample_text += (sample_text.empty() ? "" : ",") + Hex(samples[i]);
            }
        }
        if (sample_text.empty()) {
            sample_text = "none";
        }
        char line[768];
        std::snprintf(
            line, sizeof line,
            "SOTC XPRMEM MASK: %llu calls (%.2f/s), %llu masks, %llu distinct 64 KiB pages "
            "(%llu page-table overflows), repeat %.1f%% of %llu compared (same masks as the owner's "
            "previous write, %llu owner overflows), %llu unreadable; samples %s; slot 0x2ed1f58 = %s "
            "in %.0f s\n",
            static_cast<unsigned long long>(mask_calls), mask_calls / window,
            static_cast<unsigned long long>(masks), static_cast<unsigned long long>(distinct),
            static_cast<unsigned long long>(page_overflow),
            compared ? 100.0 * static_cast<double>(repeats) / static_cast<double>(compared) : 0.0,
            static_cast<unsigned long long>(compared),
            static_cast<unsigned long long>(owner_overflow),
            static_cast<unsigned long long>(mask_unreadable), sample_text.c_str(),
            slot_ok ? Hex(slot).c_str() : "unreadable", seconds);
        out += line;
    }

    if (block_calls != 0 || block_unreadable != 0) {
        const uint64_t ctx = g_ctx_last.load(std::memory_order_relaxed);
        uint64_t last[4];
        for (size_t k = 0; k < 4; ++k) {
            last[k] = g_block_last[k].load(std::memory_order_relaxed);
        }
        char line[768];
        std::snprintf(
            line, sizeof line,
            "SOTC XPRMEM BLOCKS: %llu calls (%.2f/s), global object %s (equal to the frame object in "
            "%llu), +a80 %s (%llu changes), +a88 %s (%llu changes), +a90 %s (%llu changes), +a98 %s "
            "(%llu changes), table %s..%s, %llu table unreadable, %llu unreadable in %.0f s\n",
            static_cast<unsigned long long>(block_calls), block_calls / window, Hex(ctx).c_str(),
            static_cast<unsigned long long>(ctx_is_frame), Hex(last[0]).c_str(),
            static_cast<unsigned long long>(changes[0]), Hex(last[1]).c_str(),
            static_cast<unsigned long long>(changes[1]), Hex(last[2]).c_str(),
            static_cast<unsigned long long>(changes[2]), Hex(last[3]).c_str(),
            static_cast<unsigned long long>(changes[3]),
            table_min == UINT64_MAX ? "none" : Hex(table_min).c_str(),
            table_max == 0 ? "none" : Hex(table_max).c_str(),
            static_cast<unsigned long long>(table_unreadable),
            static_cast<unsigned long long>(block_unreadable), seconds);
        out += line;
    }
}

} // namespace SotcProbes
