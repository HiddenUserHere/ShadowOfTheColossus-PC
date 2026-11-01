// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the hook side of the native allocator windows (native_windows_hooks.h). The pool logic is in pc_pool.cpp
// (SotcPcPool::SetupWindows, BindRenderTargets, SetHeapKey, ClearHeapKey, WindowRelease, WindowReuse); this file only
// decides where each one is called and what the guest sees. Guest reads go through the backing view, and the loaded
// image directly (the same rule as probe_alloc.cpp).
#include "native_windows_hooks.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../hooks.h"
#include "../pc_pool.h"
#include "video_core/sotc_trace.h" // image bounds (SotcTrace::ImageBase / ImageSize)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcNativeWindowHooks {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

// ALLOC_BLOCK (0x479a0): the owner object fields the function reads (0x479a0 decompile: the enabled byte at +9).
constexpr uint64_t kOwnerEnabledOff = 0x9;
constexpr uint64_t kOwnerNameOff = 0x10; ///< char* name (FUN_000475c0 stores the literal here)
// The RTAllocator's block (0x9aef0, the request of 0x14280000 with kind 2 on the owner). The PS4 Pro size 0x23d10000
// takes the normal path (window C is 352 MiB).
constexpr uint64_t kRtBlockSize = 0x14280000;
constexpr uint32_t kRtKind = 2;
// The GPU arena and the CPU arena objects (pc_pool.cpp SetupWindows reads the same globals).
constexpr uint64_t kArenaKind2Global = 0x2dfb2b8;
constexpr uint64_t kArenaKind1Global = 0x2dfb2c0;

// Guest memory through the backing view (the GPU tracker may protect the heap), then the image directly. False when
// neither holds the whole range.
bool ReadGuest(uint64_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (runtime_memory_region(static_cast<uintptr_t>(address), &start, &end, &mapped) && mapped &&
        address >= start && address + n <= end) {
        const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(address));
        if (p) {
            std::memcpy(out, p, n);
            return true;
        }
    }
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (base != 0 && size != 0 && address >= base && n <= size && address - base <= size - n) {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), n);
        return true;
    }
    return false;
}

// True when the owner's name (char* at +0x10) is exactly `want`.
bool OwnerNameIs(uint64_t owner, const char* want) {
    uint64_t name_ptr = 0;
    if (!ReadGuest(owner + kOwnerNameOff, &name_ptr, sizeof name_ptr) || name_ptr == 0) {
        return false;
    }
    const size_t n = std::strlen(want) + 1; // with the NUL
    char raw[32] = {0};
    if (n > sizeof raw || !ReadGuest(name_ptr, raw, n)) {
        return false;
    }
    return std::memcmp(raw, want, n) == 0;
}

} // namespace

HookAction AllocPre(GuestRegs& regs) {
    const uint64_t owner = regs.rdi;
    if (owner == 0) {
        return HookAction::Continue;
    }
    // The render-target window (C): the RTAllocator's first block. The owner of that request is the RTAllocator object
    // created in 0x9aef0 (its pointer is stored at area+0x4d78 and passed as rdi of the 0x479a0 call on line 491 of
    // the decompile). BindRenderTargets binds the window once, and ignores the later calls.
    if (regs.rsi == kRtBlockSize && static_cast<uint32_t>(regs.rcx) == kRtKind && OwnerNameIs(owner, "RTAllocator")) {
        SotcPcPool::BindRenderTargets(owner);
    }
    // The game's own check: 0x479a0 returns 0 when the owner's enabled byte is clear, before any pool call. A parked
    // node is served only in the same case.
    uint8_t enabled = 0;
    if (!ReadGuest(owner + kOwnerEnabledOff, &enabled, sizeof enabled) || enabled == 0) {
        return HookAction::Continue;
    }
    // The size the function takes from the pool: rounded up to 64 KiB (0x479a0: (rsi + 0xffff) & ~0xffff).
    const uint64_t size = (regs.rsi + 0xffffu) & ~uint64_t{0xffffu};
    uint64_t node = 0;
    if (!SotcPcPool::WindowReuse(owner, size, &node)) {
        return HookAction::Continue;
    }
    regs.rax = node; // the post phase (probe_alloc AllocPost) sees the node as the call's result
    return HookAction::SkipOriginal;
}

void MemtypePre(uint64_t memtype) {
    SotcPcPool::SetHeapKey(static_cast<int64_t>(memtype));
}

void MemtypePost() {
    SotcPcPool::ClearHeapKey();
}

namespace {

// NATIVE_WINDOW_RELEASE: entry of 0x476f0 (rdi = owner, rsi = node). A block of a window is parked: its stores are
// done by WindowRelease and the rest of the function (the call to 0x1b0e0, the VA return) is skipped.
HookAction ReleaseEntry(GuestRegs& regs, void*) {
    if (!SotcPcPool::WindowRelease(regs.rdi, regs.rsi)) {
        return HookAction::Continue;
    }
    SotcProbes::NoteWindowRelease(regs.rsi);
    return HookAction::Return;
}

// NATIVE_WINDOW_SETUP: site at 0x1171f in memory_system_init (0x110a0), after the kind-2 arena store (0x11718) and
// before the GameSingleFrame ring. Both arenas exist here (kind 1 is stored earlier). Runs on every pass of the site
// (the jump at 0x115cd reaches it when the arena already existed); SetupWindows is once per session.
HookAction SetupSite(GuestRegs&, void*) {
    static bool logged = false;
    SotcPcPool::SetupWindows();
    if (!logged) {
        logged = true;
        uint64_t arena2 = 0;
        uint64_t arena1 = 0;
        const uint64_t base = SotcTrace::ImageBase();
        const bool ok2 = ReadGuest(base + kArenaKind2Global, &arena2, sizeof arena2);
        const bool ok1 = ReadGuest(base + kArenaKind1Global, &arena1, sizeof arena1);
        std::fprintf(stderr, "SOTC NATIVE WINDOWS: setup at 0x1171f: GPU arena 0x%llx (%s), CPU arena 0x%llx (%s)\n",
                     static_cast<unsigned long long>(arena2), ok2 ? "read" : "unreadable",
                     static_cast<unsigned long long>(arena1), ok1 ? "read" : "unreadable");
    }
    return HookAction::Continue;
}

} // namespace

void Register() {
    SotcHooks::Handler release;
    release.pre = &ReleaseEntry;
    SotcHooks::RegisterHandler("NATIVE_WINDOW_RELEASE", release);

    SotcHooks::Handler setup;
    setup.pre = &SetupSite;
    SotcHooks::RegisterHandler("NATIVE_WINDOW_SETUP", setup);
}

} // namespace SotcNativeWindowHooks
