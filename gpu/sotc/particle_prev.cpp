// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: particle-system count readback, plan C7 (docs/re-stutters/reports2/07-readback-consumers.md,
// section 2.3). Only the PC-native serving is left: SOTC_PARTICLE_PREV=3 (set by sotc.py). The hook
// hands each ProcessStats read to SotcNativeParticles::OnStatsRead, which may return a completed copy
// of the count array instead of the GPU array. The observe modes 1 and 2 and the UMA copy requests
// (SOTC_RB_CLASS, SOTC_RB_HOST) were removed with the native model; see the git history for them.
//
// Slot timeline of ParticleSystemPool::ProcessStats (eboot 0x1018c0), checked against the
// disassembly. Let k be the pool counter (pool+0x64) before call k. The call increments it.
//   - Dispatch: sub_928f0 at 0x10197d writes the count pass into slot k%3 (0x101916-0x10197d). The
//     array address is the descriptor's lo dword | (hi dword & 0xfff) << 32, the same arithmetic as
//     sub_8c1b0. The entries written are (mult * (bits or 1)) >> 2 (0x101933-0x10196a), with mult at
//     desc+8 and bits from desc+4 bits 16-29. ProcessStats writes no descriptor.
//   - Read: the counter is now k+1. For k+1 >= 3 the call reads slot (k+1)%3 through sub_8c1b0
//     (0x1019af), and array[system] at 0x101a98 uses that same array in the same call.
// So slot s is dispatched in calls k = s (mod 3) and its array is first read two calls later, in
// call k+2 (never in k+1). The read in call k is of the slot dispatched in call k-2.
//
// Why no previous-frame serving: the read slot is already the oldest slot of the ring. The slot
// dispatched last is the newer one, and the GPU may still be running it. A stall-free read needs a
// port-owned copy of a finished slot: native/native_particles.cpp.
//
// Only the return address 0x1019b4 (ProcessStats) is measured. WaterRendering::ReadQueries calls
// the same accessor from 0xe4930 and is ignored.
#include <bit>
#include <cstdint>
#include <cstring>

#include "hooks.h"
#include "particle_prev.h"
#include "native/native_particles.h" // sotc: SOTC_NATIVE_PARTICLES=prev (mode 3)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (null when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kHook = "PARTICLE_COUNT_READ";
constexpr uint64_t kProcessStatsReturn = 0x1019b4; // eboot offset after "call sub_8c1b0" at 0x1019af
constexpr uint64_t kPoolCounter = 0x64;            // pool+0x64: u32 call counter (incremented before the read)
constexpr uint64_t kPoolRing = 0xf8;               // pool+0xf8: three descriptors
constexpr uint64_t kSlotStride = 0x28;             // descriptor stride
constexpr uint64_t kPoolBitmapPtr = 0x198;         // pool+0x198: u64* active-system bitmap
constexpr uint64_t kPoolBitmapWords = 0x1a0;       // pool+0x1a0: u32 number of bitmap words
constexpr uint64_t kPoolSystemLimit = 0x1a8;       // pool+0x1a8: u64 bound on the system index
constexpr uint64_t kScanWords = 256;               // extent scan cap: 16384 systems (the RB 64 KiB cap)

// Reads size bytes at a guest address through the backing view (never a direct read: the GPU page
// tracker may protect these pages). False when unmapped or when the value would cross a page.
bool ReadGuest(uint64_t address, void* out, size_t size) {
    if ((address & 0xfff) + size > 0x1000) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(address));
    if (!p) {
        return false;
    }
    std::memcpy(out, p, size);
    return true;
}

// The guest address of a count array, from its descriptor: the same arithmetic as sub_8c1b0.
bool ArrayAddress(uint64_t descriptor, uint64_t* array) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!ReadGuest(descriptor, &lo, 4) || !ReadGuest(descriptor + 4, &hi, 4)) {
        return false;
    }
    *array = uint64_t{lo} | ((uint64_t{hi} & 0xfff) << 32);
    return true;
}

// The u32 entries a dispatch of this descriptor writes: ProcessStats 0x101933-0x10196a, a 32-bit
// imul of mult (desc+8) by (bits or 1), then shr 2. bits = desc+4 bits 16-29.
bool DispatchEntries(uint64_t descriptor, uint64_t* entries) {
    uint32_t flags = 0;
    uint32_t mult = 0;
    if (!ReadGuest(descriptor + 4, &flags, 4) || !ReadGuest(descriptor + 8, &mult, 4)) {
        return false;
    }
    uint32_t bits = (flags >> 16) & 0x3fff;
    if (bits == 0) {
        bits = 1;
    }
    *entries = static_cast<uint32_t>(mult * bits) >> 2;
    return true;
}

// Systems the caller reads: one past the highest active index below pool+0x1a8 (the loop at
// 0x101a10-0x101ddc reads only those). Scans at most kScanWords bitmap words. False when unreadable.
bool ActiveExtent(uint64_t pool, uint64_t* extent) {
    uint64_t bitmap = 0;
    uint64_t limit = 0;
    uint32_t words = 0;
    if (!ReadGuest(pool + kPoolBitmapPtr, &bitmap, 8) || !ReadGuest(pool + kPoolBitmapWords, &words, 4) ||
        !ReadGuest(pool + kPoolSystemLimit, &limit, 8)) {
        return false;
    }
    const uint64_t scan = words < kScanWords ? words : kScanWords;
    uint64_t out = 0;
    for (uint64_t w = 0; w < scan; ++w) {
        uint64_t bits = 0;
        if (!ReadGuest(bitmap + 8 * w, &bits, 8)) {
            break;
        }
        while (bits != 0) {
            const uint64_t i = w * 64 + static_cast<uint64_t>(std::countr_zero(bits));
            bits &= bits - 1;
            if (i >= limit) {
                break;
            }
            if (i + 1 > out) {
                out = i + 1;
            }
        }
    }
    *extent = out;
    return true;
}

// Mode 3: hands the call's arrays to the native serving (native/native_particles.h). An unreadable
// descriptor or pool keeps the exact read.
HookAction ServeNative(GuestRegs& regs, uint64_t pool, uint64_t dispatch_desc) {
    uint32_t counter = 0;
    uint64_t extent = 0;
    if (!ReadGuest(pool + kPoolCounter, &counter, 4) || !ActiveExtent(pool, &extent)) {
        return HookAction::Continue;
    }
    uint64_t dispatch_base = 0;
    uint64_t dispatched = 0;
    if (!ArrayAddress(dispatch_desc, &dispatch_base) || !DispatchEntries(dispatch_desc, &dispatched)) {
        dispatch_base = 0;
        dispatched = 0;
    }
    return SotcNativeParticles::OnStatsRead(regs, pool, counter, dispatch_base, dispatched, extent);
}

HookAction OnCountRead(GuestRegs& regs, void*) {
    // Read once, after the hooks are installed (the first call comes from the game).
    static const int mode = SotcHooks::HookMode(kHook);
    if (mode != 3) {
        return HookAction::Continue;
    }

    // Only the ProcessStats call site. At entry, [rsp] is the return address.
    const uint64_t image = static_cast<uint64_t>(SotcHooks::ImageBase());
    uint64_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), 8);
    if (image == 0 || ret != image + kProcessStatsReturn) {
        return HookAction::Continue;
    }

    // rdi is the descriptor of the slot being read; r13 is the pool object (ProcessStats keeps
    // param_1 there). Anything else is not the pattern we checked, so leave the call alone.
    const uint64_t pool = regs.r13;
    const uint64_t read_desc = regs.rdi;
    if (read_desc < pool + kPoolRing || (read_desc - pool - kPoolRing) % kSlotStride != 0) {
        return HookAction::Continue;
    }
    const uint64_t read_slot = (read_desc - pool - kPoolRing) / kSlotStride;
    if (read_slot >= 3) {
        return HookAction::Continue;
    }

    // sotc: SOTC_NATIVE_PARTICLES=prev (sotc.py sets mode 3): the last completed copy, no observation.
    // The slot dispatched in this call is (read + 2) % 3 (see the file comment).
    return ServeNative(regs, pool, pool + kPoolRing + ((read_slot + 2) % 3) * kSlotStride);
}

} // namespace

namespace SotcProbes {

void RegisterParticlePrev() {
    SotcHooks::Handler handler;
    handler.pre = &OnCountRead;
    handler.post = nullptr;
    handler.user = nullptr;
    SotcHooks::RegisterHandler(kHook, handler);
}

} // namespace SotcProbes
