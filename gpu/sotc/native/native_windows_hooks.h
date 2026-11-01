// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the hook side of the native allocator windows (pc_pool_windows.h, docs/re-stutters/native/impl-windows.md).
// The windows are served from three existing probe handlers and from two hook sites of their own
// (scripts/hooks.d/native_windows.py, switch SOTC_NATIVE). Every entry point is inert with SOTC_NATIVE off.
#pragma once

#include <cstdint>

#include "../hooks.h"

namespace SotcNativeWindowHooks {

/// ALLOC_BLOCK pre (0x479a0), called from probe_alloc.cpp AllocPre after its frame push when SOTC_NATIVE is on.
/// Binds the render-target window to the RTAllocator's first block, then serves a parked node of the owner
/// (WindowReuse) and answers SkipOriginal: the post phase of the probe then runs with rax = the node.
SotcHooks::HookAction AllocPre(SotcHooks::GuestRegs& regs);

/// MEMTYPE_ALLOC pre and post (0x12820, a wrap), called from probe_memtype.cpp. The heap key of window B.
void MemtypePre(uint64_t memtype);
void MemtypePost();

/// Registers the handlers of the two native hooks (NATIVE_WINDOW_RELEASE, NATIVE_WINDOW_SETUP). Called from
/// RegisterAllHandlers (hook_handlers.cpp).
void Register();

} // namespace SotcNativeWindowHooks

namespace SotcProbes {
/// probe_alloc.cpp: forgets a block released to a parked window (the probe's node table and counts), without the
/// pool's free check. FREE_BLOCK does not see that release, because 0x476f0 returns before 0x1b0e0.
void NoteWindowRelease(uint64_t node);
} // namespace SotcProbes
