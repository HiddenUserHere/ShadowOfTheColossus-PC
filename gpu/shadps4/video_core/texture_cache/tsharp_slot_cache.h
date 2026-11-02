// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_bindless.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {

/// sotc: SOTC_TSLOT: T#-content caches of the texture binds. Two parts share the switch:
///  - the texture identity skip (classic and bindless): a plain sampled T# whose SOTC_GEN_TEX memo
///    entry holds skips its ImageDesc in BindTexturesFull (TextureCache::ProbeTextureIdentity);
///  - the bindless T# slot cache (needs SOTC_N5_BINDLESS): a bindless bind whose T# resolved to the
///    same live image, view info and heap slot takes that slot (Find / Store).
/// Read once. 0 off (the default), 1 on, 2 verify: the old path runs as well, its answer is used,
/// and each difference is counted. Mode 1 turns the identity memo on (SOTC_GEN_TEX is implied).
class TsharpSlotCache {
public:
    static constexpr u32 kCapacity = 16384; ///< entries, a power of two
    static constexpr u32 kWindow = 8;       ///< probe window (entries from the home index)
    static constexpr u32 kSampleEvery = 256; ///< Find times one call in this many

    /// SOTC_TSLOT (0, 1 or 2). Does not depend on SOTC_N5_BINDLESS.
    [[nodiscard]] static int Mode();

    /// The session's bindless slot cache, bound to `heap`. Null when Mode() is 0 or the heap is off.
    /// Draw thread only.
    [[nodiscard]] static TsharpSlotCache* For(BindlessImages& heap);

    struct Entry {
        std::array<u64, 4> words{}; ///< the T# dwords of the bind
        u32 key{};                  ///< heap key of the bind
        u32 slot{};                 ///< heap slot the bind got
        ImageId image_id{};
        u64 image_uid{};
        ImageViewInfo view_info{};
        vk::ImageView view{};
        u64 last_use{}; ///< stamp of the last hit or store (eviction order)
        bool valid{};
    };

    /// Probe. Returns the entry that answers the bind, or null (counted by reason). A hit touches
    /// the entry; the caller marks the heap slot used (BindlessImages::Use) and reads the entry's
    /// slot and view. Heap side effects: none (Holds only). `tsharp` is the T# the bind read.
    [[nodiscard]] Entry* Find(const AmdGpu::Image& tsharp, u32 key, ImageId image_id, u64 image_uid,
                              const ImageViewInfo& view_info, vk::ImageLayout layout);

    /// Records the answer of a full bind. Overwrites the entry of the same T# and key, or takes an
    /// empty entry of the window, or evicts the least recently used one (its heap slot is released).
    void Store(const AmdGpu::Image& tsharp, u32 key, ImageId image_id, u64 image_uid,
               const ImageViewInfo& view_info, const BindlessRef& ref);

    /// Counts a verify-mode comparison of a bindless hit with the old resolution.
    void NoteVerify(bool same);

    /// Counts a texture identity skip probe (hit or not), and a verify-mode comparison of one.
    static void NoteTexSkip(bool hit);
    static void NoteTexVerify(bool same);

    /// One line for the 5 s report (counters are reset on read):
    /// "SOTC TSLOT tex lookups N hits N verify N mismatch N slot lookups N hits N miss none N image N
    /// view N slot N stores N evict N verify N mismatch N ns/s N". The ns/s value is estimated from
    /// the sampled calls. Returns the length written, 0 when Mode() is 0. Reads atomics only.
    [[nodiscard]] static u32 Report(char* out, u32 size, double seconds);

private:
    explicit TsharpSlotCache(BindlessImages& heap_);

    BindlessImages* heap;
    std::vector<Entry> entries;
    u64 clock{};

    std::atomic<u64> lookups{};
    std::atomic<u64> hits{};
    std::atomic<u64> miss_none{};
    std::atomic<u64> miss_image{};
    std::atomic<u64> miss_view{};
    std::atomic<u64> miss_slot{};
    std::atomic<u64> stores{};
    std::atomic<u64> evictions{};
    std::atomic<u64> verifies{};
    std::atomic<u64> mismatches{};
    std::atomic<u64> sampled_ns{};
};

} // namespace VideoCore
