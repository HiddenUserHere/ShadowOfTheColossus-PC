// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Entry hooks: redirect a guest function entry to a host callback.
//
// The native render rewrite (docs/engine/native-render-map.md) replaces the game's RenderJob
// functions with C++. Each replaced function needs its entry detoured to the host implementation,
// and (until Phase 3) the original must stay reachable so a single SOTC_RENDER switch can fall back.
//
// This is the "hook infrastructure" that upstream/main lacks (gap N0-2): a registry of
// {guest VA -> host callback} applied to the mapped eboot before it runs, reusing the trampoline
// area and the Xbyak code generator the CPU patchers already use.
//
// Mechanism: at a function entry, write a 14-byte absolute indirect jump
//     FF 25 00 00 00 00  <u64 host>
// (jmp qword ptr [rip+0]) — needs no nearby trampoline and no rel32 range, so it works for any
// host callback regardless of distance. The 14 bytes must fit the entry's first instruction(s):
// a function whose entry is shorter than 14 bytes is relocated into a trampoline (call the host
// hook, then run the relocated prologue and jump back), exactly as cpu_patches.cpp does.
#pragma once

#include <cstddef>
#include <span>

#include "common/types.h"

namespace Core::SotcHooks {

/// One registered entry hook.
struct Hook {
    u64 guest_va;          ///< guest VA (image offset) of the function entry
    void* host_callback;   ///< the C++ replacement
    const char* name;      ///< diagnostic name ("0xcefa0 RenderThread_EndFrameViewParams")
    /// When true, the guest runs SysV (HLE-patched game): the entry jump lands on a register-
    /// transparent shim that saves/restores the SysV volatile registers around the callback
    /// (which is a normal Windows x64 function). When false (kernel/NID paths) the callback is
    /// called with the Windows ABI directly.
    bool sysv{true};
};

/// The trampoline that runs the original function body (its relocated prologue, then a jump back
/// into the function past the overwritten bytes). A callback that wants to chain to the original
/// calls this; it is set by Install().
struct InstalledHook {
    const Hook* hook;
    void* original; ///< call as the original signature to run the guest body
};

/// Installs every hook in `hooks` over the mapped image. `image` is the base pointer the eboot was
/// mapped at (guest VA == image + va for the first segment). `trampoline`/`trampoline_size` is the
/// same executable scratch area the CPU patchers use (within +-2 GiB of the image). `out` (optional)
/// receives, per installed hook, the original-entry thunk. Returns the number of hooks installed;
/// `failed` counts the entries that could not be rewritten.
u64 Install(void* image, u64 module_size, void* trampoline, u64 trampoline_size,
            std::span<const Hook> hooks, u64* failed, std::span<InstalledHook> out = {});

/// Looks up the original-entry thunk of an installed hook by guest VA (null when not installed).
void* OriginalEntry(u64 guest_va);

/// The table of RenderJob entry hooks (gpu/sotc/hook_table.inc), selected by SOTC_RENDER so only
/// the stage being worked on is detoured. Empty when SOTC_RENDER is unset.
std::span<const Hook> RenderJobHooks();

} // namespace Core::SotcHooks
