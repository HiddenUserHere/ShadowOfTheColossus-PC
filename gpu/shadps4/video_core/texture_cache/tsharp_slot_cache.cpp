// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "common/logging/log.h"
#include "video_core/texture_cache/tsharp_slot_cache.h"

namespace VideoCore {

static_assert(sizeof(AmdGpu::Image) == 4 * sizeof(u64));
static_assert((TsharpSlotCache::kCapacity & (TsharpSlotCache::kCapacity - 1)) == 0);
static_assert(TsharpSlotCache::kWindow <= TsharpSlotCache::kCapacity);
static_assert((TsharpSlotCache::kSampleEvery & (TsharpSlotCache::kSampleEvery - 1)) == 0);

// sotc: the session's bindless slot cache. Created once and never freed, so Report can read it from
// the driver thread; g_cache publishes it. Only the draw thread creates or changes its entries.
static std::unique_ptr<TsharpSlotCache> g_owner;
static std::atomic<TsharpSlotCache*> g_cache{nullptr};

// sotc: texture identity skip counters (TextureCache::ProbeTextureIdentity), reset by Report.
static std::atomic<u64> tex_lookups{};
static std::atomic<u64> tex_hits{};
static std::atomic<u64> tex_verifies{};
static std::atomic<u64> tex_mismatches{};

int TsharpSlotCache::Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_TSLOT");
        const int m = v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
        if (m != 0 && BindlessImagesMode() == 0) {
            LOG_INFO(Render_Vulkan,
                     "SOTC_TSLOT={}: texture identity skip only (SOTC_N5_BINDLESS is off, no T# slots)",
                     m);
        }
        return m;
    }();
    return mode;
}

TsharpSlotCache::TsharpSlotCache(BindlessImages& heap_) : heap{&heap_}, entries(kCapacity) {}

TsharpSlotCache* TsharpSlotCache::For(BindlessImages& heap) {
    if (Mode() == 0 || BindlessImagesMode() == 0) {
        return nullptr;
    }
    TsharpSlotCache* cache = g_cache.load(std::memory_order_acquire);
    if (!cache) {
        g_owner.reset(new TsharpSlotCache(heap)); // the constructor is private: For is a member
        cache = g_owner.get();
        g_cache.store(cache, std::memory_order_release);
        LOG_INFO(Render_Vulkan,
                 "SOTC TSLOT mode {}: T# slot cache with {} entries (window {}) on the bindless heap",
                 Mode(), kCapacity, kWindow);
    } else if (cache->heap != &heap) {
        // A new heap: the entries name slots of the old one. They are dropped, not released.
        cache->heap = &heap;
        for (Entry& e : cache->entries) {
            e.valid = false;
        }
    }
    return cache;
}

// Home index of a T# and heap key, the first slot of the probe window.
static u32 HomeIndex(const std::array<u64, 4>& words, u32 key) {
    u64 h = 0x9E3779B97F4A7C15ULL ^ key;
    for (const u64 word : words) {
        h = (h ^ word) * 0xC2B2AE3D27D4EB4FULL;
        h ^= h >> 29;
    }
    return static_cast<u32>(h ^ (h >> 32)) & (TsharpSlotCache::kCapacity - 1);
}

namespace {
// Adds the time of the scope to `total` when `on` (one call in kSampleEvery is timed; the rest read
// no clock).
struct SampledTimer {
    std::atomic<u64>& total;
    bool on;
    std::chrono::steady_clock::time_point start{};
    explicit SampledTimer(std::atomic<u64>& total_, bool on_) : total{total_}, on{on_} {
        if (on) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~SampledTimer() {
        if (on) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            total.fetch_add(static_cast<u64>(ns), std::memory_order_relaxed);
        }
    }
};
} // namespace

TsharpSlotCache::Entry* TsharpSlotCache::Find(const AmdGpu::Image& tsharp, u32 key,
                                              ImageId image_id, u64 image_uid,
                                              const ImageViewInfo& view_info,
                                              vk::ImageLayout layout) {
    const u64 n = lookups.fetch_add(1, std::memory_order_relaxed);
    const SampledTimer timed{sampled_ns, (n & (kSampleEvery - 1)) == 0};
    std::array<u64, 4> words{};
    std::memcpy(words.data(), &tsharp, sizeof(tsharp));

    const u32 home = HomeIndex(words, key);
    Entry* found = nullptr;
    for (u32 i = 0; i < kWindow && !found; ++i) {
        Entry& e = entries[(home + i) & (kCapacity - 1)];
        if (e.valid && e.key == key && e.words == words) {
            found = &e; // unique per (T#, key): Store overwrites instead of adding
        }
    }
    if (!found) {
        miss_none.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // The image the bind resolved to must be the image the entry resolved to (same live object).
    if (found->image_id != image_id || found->image_uid != image_uid) {
        miss_image.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (!(found->view_info == view_info)) {
        miss_view.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // The heap must still hold that view, in that layout, in that slot (destroyed or taken away
    // views release their slot, a layout change takes a new one).
    if (!heap->Holds(key, found->slot, found->view, layout)) {
        miss_slot.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    found->last_use = ++clock;
    hits.fetch_add(1, std::memory_order_relaxed);
    return found;
}

void TsharpSlotCache::Store(const AmdGpu::Image& tsharp, u32 key, ImageId image_id, u64 image_uid,
                            const ImageViewInfo& view_info, const BindlessRef& ref) {
    std::array<u64, 4> words{};
    std::memcpy(words.data(), &tsharp, sizeof(tsharp));

    const u32 home = HomeIndex(words, key);
    Entry* target = nullptr;
    Entry* empty = nullptr;
    Entry* lru = nullptr;
    for (u32 i = 0; i < kWindow && !target; ++i) {
        Entry& e = entries[(home + i) & (kCapacity - 1)];
        if (e.valid && e.key == key && e.words == words) {
            target = &e;
        } else if (!e.valid) {
            if (!empty) {
                empty = &e;
            }
        } else if (!lru || e.last_use < lru->last_use) {
            lru = &e;
        }
    }
    if (!target) {
        if (empty) {
            target = empty;
        } else {
            // Full window: the least recently used entry gives its heap slot back. The slot is
            // cleared only after the GPU has passed the tick of the release (BindlessImages::Release),
            // so a submitted draw that reads it is not affected. Same slot as the new answer: kept.
            target = lru;
            evictions.fetch_add(1, std::memory_order_relaxed);
            if (lru->key != key || lru->slot != ref.slot) {
                heap->Release(lru->key, lru->slot, lru->view);
            }
        }
    }
    target->words = words;
    target->key = key;
    target->slot = ref.slot;
    target->image_id = image_id;
    target->image_uid = image_uid;
    target->view_info = view_info;
    target->view = ref.view;
    target->last_use = ++clock;
    target->valid = true;
    stores.fetch_add(1, std::memory_order_relaxed);
}

void TsharpSlotCache::NoteVerify(bool same) {
    verifies.fetch_add(1, std::memory_order_relaxed);
    if (!same) {
        mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

void TsharpSlotCache::NoteTexSkip(bool hit) {
    tex_lookups.fetch_add(1, std::memory_order_relaxed);
    if (hit) {
        tex_hits.fetch_add(1, std::memory_order_relaxed);
    }
}

void TsharpSlotCache::NoteTexVerify(bool same) {
    tex_verifies.fetch_add(1, std::memory_order_relaxed);
    if (!same) {
        tex_mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

u32 TsharpSlotCache::Report(char* out, u32 size, double seconds) {
    if (Mode() == 0) {
        return 0;
    }
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const u64 tl = take(tex_lookups);
    const u64 th = take(tex_hits);
    const u64 tv = take(tex_verifies);
    const u64 tm = take(tex_mismatches);
    TsharpSlotCache* cache = g_cache.load(std::memory_order_acquire);
    u64 lookups_n = 0, hits_n = 0, none_n = 0, image_n = 0, view_n = 0, slot_n = 0, stores_n = 0;
    u64 evict_n = 0, verify_n = 0, mismatch_n = 0, ns_sampled = 0;
    if (cache) {
        lookups_n = take(cache->lookups);
        hits_n = take(cache->hits);
        none_n = take(cache->miss_none);
        image_n = take(cache->miss_image);
        view_n = take(cache->miss_view);
        slot_n = take(cache->miss_slot);
        stores_n = take(cache->stores);
        evict_n = take(cache->evictions);
        verify_n = take(cache->verifies);
        mismatch_n = take(cache->mismatches);
        ns_sampled = take(cache->sampled_ns);
    }
    // One call in kSampleEvery is timed: the sum is scaled up to every call.
    const double ns_per_s = seconds > 0.0 ? static_cast<double>(ns_sampled) * kSampleEvery / seconds
                                          : 0.0;
    const int n = std::snprintf(
        out, size,
        "SOTC TSLOT tex lookups %llu hits %llu verify %llu mismatch %llu slot lookups %llu hits %llu "
        "miss none %llu image %llu view %llu slot %llu stores %llu evict %llu verify %llu mismatch "
        "%llu ns/s %.0f",
        static_cast<unsigned long long>(tl), static_cast<unsigned long long>(th),
        static_cast<unsigned long long>(tv), static_cast<unsigned long long>(tm),
        static_cast<unsigned long long>(lookups_n), static_cast<unsigned long long>(hits_n),
        static_cast<unsigned long long>(none_n), static_cast<unsigned long long>(image_n),
        static_cast<unsigned long long>(view_n), static_cast<unsigned long long>(slot_n),
        static_cast<unsigned long long>(stores_n), static_cast<unsigned long long>(evict_n),
        static_cast<unsigned long long>(verify_n), static_cast<unsigned long long>(mismatch_n),
        ns_per_s);
    return n < 0 ? 0 : static_cast<u32>(n);
}

} // namespace VideoCore
