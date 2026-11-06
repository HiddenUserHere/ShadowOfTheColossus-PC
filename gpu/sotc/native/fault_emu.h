// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_NATIVE_TRACK_EMU=1 (with SOTC_NATIVE_TRACK=3). Write faults on GPU-written pages emulated in the handler.
//
// Mode 3 write-protects the pages the GPU wrote. A CPU write there faults, and the handler merges: it waits for the
// GPU, copies the page's GPU bytes into guest memory and lets the write land (about 81 us per fault, almost all of it
// the merge). With this switch the faulting store is emulated instead, the "slowmem" path of emulators with fastmem:
// the instruction is decoded, its store is done through the backing view (which the page protection does not cover),
// the written bytes are announced (the drain uploads exactly those bytes over the GPU's, BufferCache::NativeExactUpload)
// and RIP moves past it. The page stays protected and GPU-written, nothing is read back. Stores the emulator does not
// know (string moves, AVX, segment overrides, two memory operands) take the merge path as before.
#pragma once

#include <cstdint>

namespace SotcFaultEmu {

/// SOTC_NATIVE_TRACK_EMU (read once): 0 off, 1 emulate at the fault, 2 also hook the hot sites live (a Site detour,
/// or the whole function relocated when the detour is refused) and keep them in SOTC_HOOKS_LEARNED
/// (default user/hooks_learned.bin), installed again at the next boot.
int Mode();
/// Mode() != 0 and SOTC_NATIVE_TRACK=3.
bool On();

/// Whether [addr, addr + size) holds GPU-written bytes (the buffer cache's page bits). Set by the page manager.
using GpuQuery = bool (*)(uint64_t addr, uint64_t size);
void SetGpuQuery(GpuQuery query);

/// Whether the GPU finished the work of a scheduler tick. Set by the buffer cache's drain.
using TickQuery = bool (*)(void* ctx, uint64_t tick);
void SetTickQuery(TickQuery query, void* ctx);
/// The drain copied the guest bytes of [addr, addr + size) to the GPU (copy_ns: steady clock before the copy) in the
/// work of tick. An emulated store older than the copy stops being pending once that tick finished (plus a grace),
/// so a readback recorded before the copy cannot bring older bytes over it.
void NoteDrained(uint64_t addr, uint64_t size, uint64_t tick, int64_t copy_ns);

/// Boot, after the hooks of out/hooks.bin (sotc_patches.cpp): installs the learned store hooks (mode 2).
void InstallLearned(uint64_t image_base);

/// Guest thread, in the write-fault handler (context: EXCEPTION_POINTERS*). Emulates the store at the fault's RIP
/// when it writes fault_addr and is one the emulator knows. True: done, RIP advanced, the fault is handled.
bool TryEmulate(void* context, uint64_t fault_addr);

/// Calls fn(addr, size) for each part of [addr, addr + size) that holds no pending emulated store. An emulated store
/// lives in guest memory only until the drain copies it to the GPU and that copy ran: a readback of the page (the
/// merge of a store the emulator refused, any other readback) must not put the GPU's older bytes over it before.
template <typename Fn>
void ForEachNotPending(uint64_t addr, uint64_t size, Fn&& fn);

namespace Detail {
/// The pending emulated stores intersecting [addr, addr + size), sorted, clipped to it (pruned of old ones).
void PendingIn(uint64_t addr, uint64_t size, uint64_t* out, uint32_t* count, uint32_t max);
bool AnyPending();
} // namespace Detail

template <typename Fn>
void ForEachNotPending(uint64_t addr, uint64_t size, Fn&& fn) {
    if (!On() || !Detail::AnyPending()) {
        fn(addr, size);
        return;
    }
    constexpr uint32_t kMax = 64;
    uint64_t ranges[kMax * 2];
    uint64_t cursor = addr;
    const uint64_t end = addr + size;
    for (;;) { // in batches of kMax pending stores
        uint32_t count = 0;
        Detail::PendingIn(cursor, end - cursor, ranges, &count, kMax);
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t lo = ranges[i * 2];
            const uint64_t hi = ranges[i * 2 + 1];
            if (lo > cursor) {
                fn(cursor, lo - cursor);
            }
            cursor = hi > cursor ? hi : cursor;
        }
        if (count < kMax || cursor >= end) {
            break;
        }
    }
    if (cursor < end) {
        fn(cursor, end - cursor);
    }
}

} // namespace SotcFaultEmu
