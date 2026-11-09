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

/// sotc: out of line: a GPU write marked [addr, addr + size) GPU-modified (see NoteGpuWrite).
void GpuWriteRange(VAddr addr, u64 size) noexcept;
/// sotc: out of line: a formatted (texel) read inside Touch copied an aliasing image into the arena (counted).
void TexelImageSynced() noexcept;

/// sotc: the buffer cache (AddGpuModified) or the texture cache (MarkGpuModified) marked a range GPU-modified. Its
/// placed pages lose their clean bit and get a GPU-modified mark, so no fast path (Touch, Buf::BindAddressed) answers
/// them before a slow Touch has synchronized them again. Ignored inside Touch's own ObtainBuffer (that sync is the one
/// that sets the bits). Table mode only: one relaxed load while the table is off.
inline void NoteGpuWrite(VAddr addr, u64 size) noexcept {
    if (g_hooks_on.load(std::memory_order_relaxed) && size != 0) {
        GpuWriteRange(addr, size);
    }
}

/// sotc: SynchronizeMemoryFromImage copied an image into the arena (counted only inside Touch, table mode only).
inline void NoteTexelImageSync() noexcept {
    if (g_hooks_on.load(std::memory_order_relaxed)) {
        TexelImageSynced();
    }
}

} // namespace SotcArch::AddrTable
