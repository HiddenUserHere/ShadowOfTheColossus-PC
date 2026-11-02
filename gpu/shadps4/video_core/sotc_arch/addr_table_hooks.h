// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the light part of the SOTC_ADDR_TABLE clean state (addr_table.h), for the hooks in the memory tracker and the
// region manager, which must not pull in the Vulkan headers. Dirty clears the clean state of table pages; it is lock-free
// and costs one relaxed load while the table is off.
#pragma once

#include <atomic>

#include "common/types.h"

namespace SotcArch::AddrTable {

/// True once the table exists (set by Init, before any guest thread runs).
extern std::atomic<bool> g_hooks_on;

/// Out of line: clears the clean state of the table pages of [addr, addr + size) (any thread, no lock).
void DirtyRange(VAddr addr, u64 size) noexcept;
/// Out of line: the same for the 4 KiB tracking pages whose bits are set in mask; bit i is page base + 4096 * i.
void DirtyPages(VAddr base, u64 mask) noexcept;

/// A guest range whose bytes changed, or whose device placement changed: its pages are no longer clean.
inline void Dirty(VAddr addr, u64 size) noexcept {
    if (g_hooks_on.load(std::memory_order_relaxed) && size != 0) {
        DirtyRange(addr, size);
    }
}

/// Region-manager form of Dirty: a 64-bit tracking word at base (4 KiB pages), the bits that changed.
inline void DirtyMask(VAddr base, u64 mask) noexcept {
    if (g_hooks_on.load(std::memory_order_relaxed) && mask != 0) {
        DirtyPages(base, mask);
    }
}

} // namespace SotcArch::AddrTable
