// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_STATIC_CLASS, the GPU_STATIC class of resource-build allocations (static_class.cpp,
// docs/re-stutters/PC-MEMORY-REWRITE.md). Hooks are declared in scripts/hooks.d/staticclass.py.
#pragma once

#include <cstdint>
#include <string>

namespace SotcStaticClass {

/// Registers the handlers of the four STATIC_CLASS_* hooks. Called from RegisterAllHandlers()
/// (hook_handlers.cpp), before InstallHooks.
void Register();

/// Appends the "SOTC STATIC CLASS: ..." line (and, in mode 3, a "SOTC STATIC SKIP: ..." line) of
/// the last report window to out and resets the counters. Nothing is appended when
/// SOTC_STATIC_CLASS is 0. seconds = window length.
void Report(std::string& out, double seconds);

// SOTC_STATIC_CLASS=3: apply, and static mesh ranges skip the per-bind sync scan while they stay
// clean. Only the buffer cache calls these (BufferCache::SynchronizeMemory, see static_class.cpp
// "Mode 3"); all three do nothing unless Skip3Enabled() is true.

/// True when SOTC_STATIC_CLASS=3 and the sync generation stamps are on (SOTC_SYNC_FAST=1 or
/// SOTC_BUF_BIND_FAST=1). Without the stamps the skip could not see a rewrite, so =3 runs as =1.
bool Skip3Enabled();

/// True when the read of [addr, addr + size) may skip the page scan: the exact range was armed by
/// NoteClean and the sync generation sum gen0 (loaded by the caller for this range, before any
/// check) equals the one recorded at arm time. A changed sum disarms the range (a rewrite).
bool TrySkip(uint64_t addr, uint64_t size, uint64_t gen0);

/// The full sync of [addr, addr + size) found it clean (no CPU bit, scan_clean) with gen0 loaded
/// before that scan. Arms the range when it lies in a registered static allocation.
void NoteClean(uint64_t addr, uint64_t size, uint64_t gen0);

} // namespace SotcStaticClass
