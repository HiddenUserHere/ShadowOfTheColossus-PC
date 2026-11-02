// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BINDSET event API (stage 1, ARCH-BINDSET). A BindSet is a table memo entry (table_memo.cpp) that is replayed
// without validating it: the state it depends on is watched through these events instead. Each event names an image
// (by its texture cache index), a heap slot (by its key and slot) or the sampler cache. The events are queued from any
// thread and applied by the draw thread at the start of the next TableMemo::Resolve, before any entry is looked up.
//
// Posting is thread-safe. When SOTC_BINDSET is off (or the heap is off), Armed() is false and the posts do nothing.
#pragma once

#include "common/types.h"

namespace SotcArch::BindSet {

/// Why an event kills the entries that depend on it (also the order of the kill counters).
enum class Cause : u8 {
    ImageFree,   ///< the image was unregistered, parked, unparked (new identity) or its slot freed
    ImageDirty,  ///< a dirty flag was set (CPU write, GPU write-back, deferred upload)
    ImageTrack,  ///< the image's tracked range changed (a page of it was untracked or tracked again)
    ImageRebind, ///< needs_rebind was set on the image
    ImageDepth,  ///< the depth association of a stencil image was made
    ImageSet,    ///< an image was registered over this one (a new candidate for the same guest address)
    HeapSlot,    ///< a bindless heap slot was released or evicted
    Sampler,     ///< a sampler was destroyed (the sampler generation moved)
    Range,       ///< a mip-tail candidate's guest range changed: an image over it was registered, unregistered,
                 ///< or GPU-written for the first time (or lost its GPU-written mark)
    Count,
};

/// True when posts are applied (SOTC_BINDSET on and the table memo is up). Cheap: one atomic load.
bool Armed() noexcept;
/// An image index (ImageId::index) had a cause that its replays depend on. Not queued when no live replay links the
/// image (the link gate, table_memo.cpp): the state change made before this call is what the gate reads.
void PostImage(u32 image_index, Cause cause);
/// A bindless heap slot of `key` was released or evicted. Not queued when no live replay holds the slot.
void PostSlot(u32 key, u32 slot);
/// The sampler cache destroyed a sampler.
void PostSamplers();
/// The guest bytes [lo, hi) changed in a way the mip-tail candidates overlapping them depend on (Cause::Range).
/// Not queued while no candidate replay is live.
void PostRange(u64 lo, u64 hi);

} // namespace SotcArch::BindSet
