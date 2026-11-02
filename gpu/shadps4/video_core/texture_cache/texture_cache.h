// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <cstdlib>
#include <optional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <boost/container/small_vector.hpp>
#include <deque>
#include <memory>
#include <queue>
#include <string>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "video_core/sotc_stats.h"
#include "video_core/range_stamps.h"
#include "common/multi_level_page_table.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_prefetch.h"
#include "video_core/texture_cache/image_view.h"
#include "../../../sotc/area_prewarm.h" // sotc: SOTC_AREA_PREWARM replay records
#include "../../../sotc/mem_classes.h"   // sotc: SOTC_CLASS_TRACK bind signal
#include "../../../sotc/pc_pool_place_glue.h" // sotc: SOTC_PC_PLACE bind evidence
#include "../../../sotc/native/n1verify.h" // sotc: SOTC_VERIFY=n1 (NoteBind)
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

/// sotc: SOTC_MIP_TAIL: 0 off, 1 mip-tail images for T#s with LOD streaming counters, 2 (default)
/// for every read-only colour T# whose base_level is above 0 (TextureCache::DecideRebase). Mode 2
/// left out 2.5 GB of never-sampled mips in the attract demo: peak video memory 6.1 -> 3.3-3.5 GB
/// on a 6 GB card, frames over 50 ms 150 -> 102-112, the worst streaming frame 460 -> 250-310 ms.
inline u32 MipTailMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_MIP_TAIL");
        return v && (v[0] == '0' || v[0] == '1' || v[0] == '2') ? static_cast<u32>(v[0] - '0')
                                                                  : 2u;
    }();
    return mode;
}

/// sotc: B4 (SOTC_FPS): why an image leaves the cache (FreeImage) or is parked (ParkImage). Each
/// reason has a counter, printed every 5 s by TextureCache::ReportImageFrees.
enum class ImageLeave : u32 {
    Unmap,          ///< UnmapMemory: the guest unmapped the range (report 13)
    OverlapBlock,   ///< ResolveOverlap, equal address, other block size or dimensions (a pool)
    OverlapReuse,   ///< ResolveOverlap, equal address, other texture size (SOTC_REUSE_SIZE_CHECK)
    OverlapTiling,  ///< ResolveOverlap, equal address, other tile mode
    OverlapChance,  ///< ResolveOverlap, the cached image lies on the right: a chance overlap
    OverlapRtSub,   ///< ResolveOverlap, left side: a render target subresource
    OverlapCopyMip, ///< ResolveOverlap, left side: a subresource copied into the merged image
    Expand,         ///< ExpandImage: replaced by a larger image
    ResolveFail,    ///< FindImage: the resolved image has too few resources
    Emergency,      ///< EmergencyCollect
    Routine,        ///< GarbageCollectImages (routine pass or pressure)
    MipTail,        ///< FreeRebased: mip tails (SOTC_MIP_TAIL)
    DepthNoTwin,    ///< ResolveDepthOverlap with SOTC_DEPTH_TWIN=0
    DepthTwin,      ///< ParkImage: depth/colour twin (a park, not a free)
    UnmapPark,      ///< ParkImage: parked by UnmapMemory (SOTC_UNMAP_PARK=1, a park, not a free)
    Count,
};
inline constexpr size_t NumImageLeave = static_cast<size_t>(ImageLeave::Count);

/// sotc: SOTC_UNMAP_PARK (C5): 0 off (default). 1: UnmapMemory parks the idle images of the range
/// (ParkImage) and FindImage re-arms a parked image with the same address and shape instead of
/// creating one. 2: log only, counts what 1 would park and re-arm; nothing else changes.
inline u32 UnmapParkMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_UNMAP_PARK");
        return v && (v[0] == '1' || v[0] == '2') ? static_cast<u32>(v[0] - '0') : 0u;
    }();
    return mode;
}
/// sotc: SOTC_UNMAP_PARK_MB (default 128): the video memory the parked images may hold.
inline u64 UnmapParkBudget() {
    static const u64 bytes = [] {
        const char* v = std::getenv("SOTC_UNMAP_PARK_MB");
        return (v ? std::strtoull(v, nullptr, 10) : 128ull) << 20;
    }();
    return bytes;
}
/// sotc: the most parked images (and the most shapes mode 2 remembers); the oldest go first.
inline constexpr size_t UnmapParkMaxImages = 256;
/// sotc: parked images older than this many GC ticks are reclaimed (GarbageCollectImages).
inline constexpr u64 UnmapParkMaxAgeTicks = 300;

/// sotc: the texture cache lock, re-entrant on its owner thread. The command processor holds it
/// over a whole binding pass (Rasterizer::BindTextures, BeginRendering, PrepareRenderState), so the
/// FindImage and UpdateImage calls inside (two per texture, ~62,000 per frame at the Shrine) only
/// count their depth. SOTC_TC_BATCH=0 drops the outer holds (every call locks as before).
///
/// It is also biased to the command processor (the first thread to take a batch hold): there it
/// is a plain store and load. Any atomic read-modify-write on that thread waits for its pending
/// stores to drain (write-combined staging copies among them): the uncontended std::mutex, and
/// then a spinlock's xchg, each cost ~5% of the thread at ~10,000 holds per frame. Other threads
/// (guest write faults, ~200 per frame) take a spinlock, announce themselves and run an
/// asymmetric barrier (FlushProcessWriteBuffers) before waiting for the command processor to
/// leave: either its store of fast_held is visible to them after the barrier, or its following
/// load of slow_wants sees theirs. SOTC_TC_BIASED=0: every thread takes the spinlock.
class alignas(64) ReentrantMutex {
public:
    /// Makes the calling thread the biased owner (once).
    void SetFastThread() {
        if (!fast_token.load(std::memory_order_relaxed) && Biased() && !HeldByThisThread()) {
            // Under the spinlock: a thread holding it read fast_token as null and skipped the
            // announcement, so the biased path must not start before it leaves.
            RawLock();
            fast_token.store(Token(), std::memory_order_relaxed);
            word.store(0, std::memory_order_release);
        }
    }
    void lock() {
        const void* self = Token();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return;
        }
        if (self == fast_token.load(std::memory_order_relaxed)) {
            if (FastTryAcquire()) {
                mode = Mode::Fast;
            } else {
                RawLock(); // a guest holds it or is waiting for us: queue behind it
                mode = Mode::FastSlow;
            }
        } else {
            RawLock();
            AnnounceAndWait();
            mode = Mode::Slow;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
    }
    bool try_lock() {
        const void* self = Token();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return true;
        }
        if (self == fast_token.load(std::memory_order_relaxed)) {
            if (!FastTryAcquire()) {
                return false;
            }
            mode = Mode::Fast;
        } else {
            if (!RawTryLock()) {
                return false;
            }
            if (!AnnounceAndTry()) {
                return false;
            }
            mode = Mode::Slow;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
        return true;
    }
    bool HeldByThisThread() const {
        return owner.load(std::memory_order_relaxed) == Token();
    }
    void unlock() {
        if (--depth != 0) {
            return;
        }
        owner.store(nullptr, std::memory_order_relaxed);
        switch (mode) {
        case Mode::Fast:
            fast_held.store(0, std::memory_order_release);
            break;
        case Mode::Slow:
            slow_wants.store(0, std::memory_order_release);
            word.store(0, std::memory_order_release);
            break;
        case Mode::FastSlow:
            word.store(0, std::memory_order_release);
            break;
        }
    }

private:
    enum class Mode : u8 { Fast, Slow, FastSlow };

    static bool Biased() {
        static const bool on = [] {
            const char* v = std::getenv("SOTC_TC_BIASED");
            return !v || v[0] != '0';
        }();
        return on;
    }
    /// Only the owner stores its own token, and clears it before unlocking, so a thread reads
    /// its token back exactly while it holds the lock.
    static const void* Token() {
        static thread_local char token;
        return &token;
    }
    /// FlushProcessWriteBuffers on Windows, a full fence elsewhere (texture_cache.cpp).
    static void AsymmetricBarrier();
    static void Pause(u32& spins) {
        if (++spins < 4096) {
            __builtin_ia32_pause();
        } else {
            std::this_thread::yield();
        }
    }
    bool FastTryAcquire() {
        fast_held.store(1, std::memory_order_relaxed);
#ifdef _WIN32
        std::atomic_signal_fence(std::memory_order_seq_cst); // the barrier is on the other side
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
        if (slow_wants.load(std::memory_order_acquire) == 0) {
            return true;
        }
        fast_held.store(0, std::memory_order_release);
        return false;
    }
    void AnnounceAndWait() {
        if (!fast_token.load(std::memory_order_relaxed)) {
            return; // no biased owner (SOTC_TC_BIASED=0, or before the first batch hold)
        }
        slow_wants.store(1, std::memory_order_relaxed);
        AsymmetricBarrier();
        for (u32 spins = 0; fast_held.load(std::memory_order_acquire);) {
            Pause(spins);
        }
    }
    bool AnnounceAndTry() {
        if (!fast_token.load(std::memory_order_relaxed)) {
            return true;
        }
        slow_wants.store(1, std::memory_order_relaxed);
        AsymmetricBarrier();
        if (!fast_held.load(std::memory_order_acquire)) {
            return true;
        }
        slow_wants.store(0, std::memory_order_release);
        word.store(0, std::memory_order_release);
        return false;
    }
    void RawLock() {
        if (!word.exchange(1, std::memory_order_acquire)) {
            return;
        }
        for (u32 spins = 0;;) {
            while (word.load(std::memory_order_relaxed)) {
                Pause(spins);
            }
            if (!word.exchange(1, std::memory_order_acquire)) {
                return;
            }
        }
    }
    bool RawTryLock() {
        return !word.load(std::memory_order_relaxed) &&
               !word.exchange(1, std::memory_order_acquire);
    }
    std::atomic<u32> word{0};       ///< spinlock of the other threads (and of the biased owner
                                    ///< when it had to queue behind them)
    std::atomic<u32> slow_wants{0}; ///< a thread holding `word` waits for or holds the lock
    std::atomic<const void*> fast_token{nullptr};
    std::atomic<const void*> owner{nullptr};
    u32 depth = 0;
    Mode mode = Mode::Fast;
    alignas(64) std::atomic<u32> fast_held{0}; ///< the biased owner holds it (written by it only)
};

inline bool TextureLockBatching() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_TC_BATCH");
        return !v || v[0] != '0';
    }();
    return on;
}

/// sotc: SOTC_TEX_PROTECT_BATCH=1 (off by default). Opens a PageManager::ProtectBatch on the
/// calling thread for the texture tracking protects made inside it (TrackImage of a dirty image
/// only, see TexDeferOk; untracks are applied at once). Every guest read of the texture cache
/// (RefreshImage upload, DownloadImageMemory, SotcGlyphHook) flushes the pending protects first,
/// and so does the close of the scope. Unset: the scope does nothing. Used around each draw and
/// dispatch of the rasterizer (the ordering note is above the helpers in texture_cache.cpp).
class TexProtectScope {
public:
    TexProtectScope();
    ~TexProtectScope();
    TexProtectScope(const TexProtectScope&) = delete;
    TexProtectScope& operator=(const TexProtectScope&) = delete;

    /// Applies the protects collected so far by this scope (the scope stays open).
    void Flush();

private:
    struct Impl; // holds the PageManager::ProtectBatch (texture_cache.cpp)
    std::unique_ptr<Impl> impl;
    TexProtectScope* prev = nullptr;
};

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t ADDRESS_SPACE_BITS = 40;
        static constexpr size_t L1_BITS = 10;
        static constexpr size_t PAGE_BITS = 20;
        static constexpr bool NULL_CHECK = true;
    };
    using PageTable = Common::MultiLevelPageTable<Traits>;

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        // sotc: mip-tail candidate (SOTC_MIP_TAIL, DecideRebase): the T#'s base_level (0: not a
        // candidate) and min_lod, whether `info` holds the tail now, and the other info (the
        // tail while `info` is the full chain, the full chain while `info` is the tail).
        u8 rebase_mip{};
        bool rebased_now{};
        u16 orig_min_lod{};
        std::optional<ImageInfo> other_info;

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {
            InitMipTail(image, desc);
        }
        void InitMipTail(const AmdGpu::Image& image, const Shader::ImageResource& resource);
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    /// sotc: SOTC_RAM_REPORT: host bytes of the image and view slots (Image and ImageView objects,
    /// by count), the page table's second-level pages and the page_images index. The slot counts
    /// are read without the lock (diagnostic only).
    [[nodiscard]] u64 SotcHostBytes() const noexcept {
        return slot_images.size() * sizeof(Image) + slot_image_views.size() * sizeof(ImageView) +
               page_table.SotcAllocatedBytes() + NumImagePages * sizeof(u32);
    }

    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
    ~TextureCache();

    /// sotc: true when an image is registered in the 1 MiB block holding addr (lock free; used by
    /// SOTC_FAULT_GRANULE to leave texture memory to the per-page fault path).
    [[nodiscard]] bool HasImagesNear(VAddr addr) const {
        const u64 page = addr >> Traits::PAGE_BITS;
        return page >= NumImagePages || page_images[page].load(std::memory_order_acquire) != 0;
    }
    /// sotc: any registered image on a page of [addr, addr + size) (lock-free, approximate: for
    /// large ranges every 16th page is looked at, plus the last one).
    [[nodiscard]] bool HasImagesInRange(VAddr addr, u64 size) const {
        const u64 first = addr >> Traits::PAGE_BITS;
        const u64 last = (addr + std::max<u64>(size, 1) - 1) >> Traits::PAGE_BITS;
        const u64 step = last - first > 256 ? 16 : 1;
        for (u64 page = first; page <= last; page += step) {
            if (page >= NumImagePages || page_images[page].load(std::memory_order_acquire) != 0) {
                return true;
            }
        }
        return last < NumImagePages && page_images[last].load(std::memory_order_acquire) != 0;
    }

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// sotc: SOTC_RS_MEMO: moves whenever a CMASK/HTILE clear bit may have been set (ClearMeta,
    /// TouchMeta with is_clear, a new metadata surface). While it stands still, a slice whose
    /// bit a draw cleared is still clear.
    u64 MetaEpoch() const noexcept {
        return meta_epoch.load(std::memory_order_relaxed);
    }

    /// sotc: SOTC_RS_MEMO: what FindImage, UpdateImage and FindRenderTarget/FindDepthTarget
    /// leave on a target that an earlier draw of this tick found and refreshed, when the image is
    /// still clean, tracked and registered (the caller checks): the access stamps, the LRU touch,
    /// the GPU-written mark, the linear readback and the usage bit.
    void ReuseTarget(ImageId image_id, bool is_depth);

    /// sotc: SOTC_BIND_CACHE_XTICK: the stamps FindImage leaves on a sampled image (access ticks
    /// and LRU touch), for a cached bind that reuses it in a later submit tick.
    void NoteBoundUse(ImageId image_id);

    /// sotc: SOTC_BIND_CACHE_FRAME. The per-range form of BindGeneration's image half: RegisterImage
    /// and UnregisterImage note the guest range of the image (with the bytes a mip tail dropped),
    /// and a cached bind asks whether anything changed over the ranges it looked up since it
    /// last checked. ImageChangeSeq() is the checkpoint to read before asking.
    [[nodiscard]] u64 ImageChangeSeq() const noexcept {
        return image_stamps.Seq();
    }
    [[nodiscard]] bool ImageChangedSince(VAddr lo, VAddr hi, u64 seq) const noexcept {
        return image_stamps.ChangedSince(lo, hi, seq);
    }
    /// sotc: SOTC_BIND_CACHE_FRAME: the sampler half of BindGeneration (a destroyed sampler leaves
    /// stale handles in cached descriptors; the sampler set is not tracked per range).
    [[nodiscard]] u64 SamplerGeneration() const noexcept {
        return sampler_epoch.load(std::memory_order_acquire);
    }

    /// sotc: changes when an image is registered/unregistered or a sampler is destroyed, i.e.
    /// when a previously bound texture descriptor may hold a stale handle (SOTC_FAST_BIND).
    u64 BindGeneration() const noexcept {
        return image_set_generation ^
               (sampler_epoch.load(std::memory_order_acquire) * 0x9E3779B97F4A7C15ULL);
    }

    /// sotc: SOTC_TEX_BLOCK. The generation of the guest address for FindImage's perfect-match memo
    /// (MemoGeneration): it moves when an image is (un)registered at that address.
    [[nodiscard]] u64 ImageAddrGeneration(VAddr address) const {
        return MemoGeneration(address);
    }
    /// sotc: SOTC_TEX_BLOCK. True when `id` is what FindImage returns for `info` through a perfect
    /// match (same address, size and format, not a smaller chain): the result then depends only on
    /// the images registered at info.guest_address.
    [[nodiscard]] bool IsPerfectFind(ImageId id, const ImageInfo& info) const;

    /// Frees cached images not used in the last few GC ticks (one per submit), regardless of
    /// memory thresholds. Called when a video memory allocation fails. Higher levels go deeper
    /// (sotc): level 0 frees up to 512 images last used 4 or more ticks ago, level 1 up to 4096
    /// last used 2 or more ticks ago, level 2 every image not used in the current tick.
    /// sotc: drop_prefetched=false keeps the images made ahead (SOTC_ASYNC_IMAGE_CREATE): the
    /// flush at the start of a load, when they are about to be used.
    void EmergencyCollect(u32 level = 0, bool drop_prefetched = true);

    /// sotc: EmergencyCollect, unless this thread is inside RefreshImage (holding `mutex`): then
    /// the collection is deferred to the next RunGarbageCollector and false is returned.
    bool TryEmergencyCollect(u32 level);

    /// Invalidates any image in the logical page range.
    /// sotc: keep_gpu_written (SOTC_NATIVE=1 announces): images the GPU wrote (render targets, depth)
    /// are left alone. Without tracking nothing reads them back to guest memory, and announces such as a
    /// whole per-frame heap chunk cover them without the CPU writing their texels, so dirtying one
    /// re-uploads stale guest bytes over the GPU's (1 Nov: Shrine facade missing, black rectangles).
    void InvalidateMemory(VAddr addr, size_t size, bool keep_gpu_written = false);

    /// Marks an image as dirty if it exists at the provided address.
    /// sotc: any_overlap marks every image overlapping the range (DMA copies and fills).
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size, bool any_overlap = false);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// sotc: SOTC_ASYNC_IMAGE_CREATE=1 (image_prefetch.h).
    [[nodiscard]] bool ImagePrefetchEnabled() const noexcept {
        return image_prefetcher != nullptr;
    }
    /// sotc: SOTC_ASYNC_IMAGE_CREATE, command processor thread only (Rasterizer::PrefetchImages):
    /// when the image a draw's T# will bind does not exist yet, queues its Vulkan creation on an
    /// image worker. Touches none of the cache's structures (no lock): only its own memo of the
    /// T#s seen, the ImageInfo caches (per thread) and the prefetcher.
    /// now: SotcStats::NowNs() read once per draw by the caller.
    void PrefetchTexture(const AmdGpu::Image& tsharp, const Shader::ImageResource& resource,
                         u64 now);
    /// sotc: SOTC_AREA_PREWARM=3 (area_prewarm.h): creates ahead the images of the area that just
    /// became current. Called by PrefetchTexture while a replay is pending or under way; same thread,
    /// no cache lock. Time-boxed: a few records (about 1 ms) per call, the rest on the next calls.
    void ReplayAreaImages();

    /// sotc: B4 (SOTC_FPS): appends one line per window: the images freed and parked by reason since
    /// the last call, the registrations, and the change in the cache count they imply (nothing
    /// is appended with SOTC_FPS unset).
    static void ReportImageFrees(std::string& out, double seconds);
    /// sotc: C5 (SOTC_UNMAP_PARK): appends one line per window since the last call (mode 1 or 2).
    static void ReportUnmapPark(std::string& out, double seconds);

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// sotc: SOTC_GEN_TEX texture identity key: the raw words of the T# (AmdGpu::Image, 32 bytes)
    /// and `extra`, every other bit the bind reads from the shader resource (is_depth, is_array,
    /// is_atomic, is_r128, constant_mip_index, mip_fallback_mode, post_op), packed by the caller.
    struct TexIdentityKey {
        std::array<u64, 4> words{};
        u64 extra{};
        bool operator==(const TexIdentityKey&) const = default;
    };
    /// sotc: what a texture bind gets: the image and its view (valid while the image lives).
    struct TexBind {
        ImageId image_id{};
        ImageView* view{};
    };
    /// sotc: SOTC_GEN_TEX (0 off, 1 on, 2 verify), read once. Off unless SotcGen::On().
    [[nodiscard]] static int TexIdentityMode();
    /// sotc: FindImage + FindTexture for a texture bind (BindingType::Texture; other types and
    /// mip-tail candidates take the plain path). A memo hit returns what the full path returns for
    /// the same key without FindImage, the overlap scan or a refresh: the image is registered,
    /// fully tracked and clean, its guest range has the SotcGen::Sum it had when it was last
    /// refreshed clean, and no image was (un)registered or aliased since (tex_epoch). Mode 2 runs
    /// the full path too and counts the hits that differ; the full path's answer is returned.
    [[nodiscard]] TexBind FindTextureIdentity(const TexIdentityKey& key, ImageDesc& desc,
                                              bool exact_fmt = false);
    /// sotc: SOTC_TSLOT: what the identity memo answers for `key` when its entry holds, as a plain
    /// bind takes it (FindTextureIdentity's hit, without the ImageDesc). Same checks (tex_epoch, uid,
    /// Dirty, warm pending, track range, GenSumImage) and side effects (touch, GPU-bind evidence) as
    /// the hit. The pre view info is not compared: `key` fixes it (ImageViewInfo reads the T# and
    /// is_written / is_depth / is_array only). `want_view` also returns the view handle. False, and
    /// nothing touched, when the entry does not hold.
    struct TexSkip {
        ImageId image_id{};
        ImageViewInfo view_info{};
        vk::ImageView view{};
    };
    [[nodiscard]] bool ProbeTextureIdentity(const TexIdentityKey& key, bool want_view, TexSkip& out);
    /// sotc: SOTC_GEN_TEX counters, appends one "SOTC TEX IDENTITY" line (nothing when off).
    static void ReportTexIdentity(std::string& out, double seconds);
    /// sotc: SOTC_RT_MAXEXTENT counters (render and depth target families), appends one
    /// "SOTC RT MAXEXTENT" line (nothing when off).
    static void ReportRtMaxExtent(std::string& out, double seconds);

    /// sotc: SOTC_LOAD_WARM trace instants (texture_cache.cpp).
    static void TraceWarmTex(const char* name, VAddr addr, u64 size);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// Retrieves an image view with the properties of the specified image id.
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the render target with specified properties
    /// sotc: update=false (BeginRendering, SOTC_RT_FAST) skips the second UpdateImage when the
    /// caller has just run it and the image needs no upload or re-tracking since.
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc,
                                              bool update = true);

    /// sotc: holds the cache lock over a binding pass (ReentrantMutex); empty with
    /// SOTC_TC_BATCH=0.
    [[nodiscard]] std::unique_lock<ReentrantMutex> BatchLock() {
        mutex.SetFastThread();
        if (!TextureLockBatching()) {
            return {};
        }
        std::unique_lock lk{mutex, std::defer_lock};
        LockCounted(lk);
        return lk;
    }

    /// sotc: locks `mutex`, counting contention (SOTC_CMD_STATS, SOTC sys line) and the draw
    /// thread's wait for it (SOTC_FPS, SotcStats::DrawWait).
    static void LockCounted(std::unique_lock<ReentrantMutex>& lk) {
        // sotc: SOTC_FPS alone only times the draw thread; other threads keep the plain lock()
        // (try_lock + lock costs a second asymmetric barrier on the biased mutex).
        // sotc: SOTC_TRACE alone also times the draw thread's wait here (its span).
        if ((SotcStats::kCmdStats ||
             ((SotcStats::kFpsLog || SotcTrace::kOn) && SotcStats::tl_role == SotcStats::RoleDraw)) &&
            !lk.mutex()->HeldByThisThread()) {
            if (SotcStats::kCmdStats) {
                SotcStats::sys_tc_locks.fetch_add(1, std::memory_order_relaxed);
            }
            if (lk.try_lock()) return;
            const SotcStats::DrawWait wait{SotcStats::WaitTcLock};
            if (!SotcStats::kCmdStats) {
                lk.lock();
                return;
            }
            SotcStats::sys_tc_contended.fetch_add(1, std::memory_order_relaxed);
            const auto start = std::chrono::steady_clock::now();
            lk.lock();
            SotcStats::sys_tc_wait_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count(),
                std::memory_order_relaxed);
            return;
        }
        lk.lock();
    }

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        std::unique_lock lk{mutex, std::defer_lock};
        LockCounted(lk);
        Image& image = slot_images[image_id];
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
        if (SotcN1Verify::On() && SotcStats::t_draw_thread) { // sotc: SOTC_VERIFY=n1: the GPU binds this image
            SotcN1Verify::NoteBind(image.info.guest_address, image.info.guest_size, true);
        }
        // sotc: SOTC_CLASS_TRACK: a bound image's guest range is GPU-bound (never CPU_ONLY).
        if (SotcMemClasses::On() && SotcStats::t_draw_thread) {
            SotcMemClasses::NoteGpuBind(image.info.guest_address, image.info.guest_size);
        }
        // sotc: SOTC_PC_PLACE bind evidence, its own gate (the glue is on).
        if (SotcPcPlaceGlue::On()) {
            SotcPcPlaceGlue::Global().OnGpuBind(image.info.guest_address, image.info.guest_size);
        }
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// sotc: SOTC_RT_MAXEXTENT (T1): render and depth targets are created with the layer count
    /// their family has been seen with, so a later request for more layers is a view, not an
    /// expand. Off by default; every helper here runs under `mutex`.
    static bool IsRtTarget(BindingType type) {
        return type == BindingType::RenderTarget || type == BindingType::DepthTarget;
    }
    [[nodiscard]] static int RtMaxExtentMode();
    /// `info` with the family's layer count when that is larger (raised). Only layer-linear guest
    /// sizes are scaled; anything else returns `info` unchanged and counts a fallback.
    [[nodiscard]] ImageInfo RtRaiseLayers(const ImageInfo& info, bool& raised);
    /// Records the family's image (created or expanded to `max_layers`) and the layers the old
    /// path would have given it (`first_layers`, the request that made it).
    void RtNoteImage(const ImageInfo& request, u32 first_layers, u32 max_layers, ImageId image_id);
    /// A request served by an image that already exists: counts the expands the old path would
    /// have made (request layers above the old image's layers) and the views of a larger image.
    void RtNoteServed(const ImageInfo& info, ImageId image_id);
    /// ExpandImage for a render or depth target, to the family's layer count when that is larger.
    [[nodiscard]] ImageId ExpandRt(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor. With bindless samplers,
    /// `bindless_slot` (when given) receives its heap slot, or 0 (the default sampler) if none.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth, u32* bindless_slot = nullptr);

    /// sotc: the global sampler heap of SOTC_BINDLESS_SAMP (disabled unless the instance has it).
    [[nodiscard]] const VideoCore::BindlessSamplers& GetBindlessSamplers() const noexcept {
        return bindless_samplers;
    }

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        // sotc: the LRU is only safe under `mutex`: unlocked, a guest unmap can unregister the
        // image (freeing its LRU node) between TouchImage's Registered check and the touch. Calls
        // outside a batch hold skip it; the locked FindImage/UpdateImage of a bound image touch it.
        if (mutex.HeldByThisThread()) {
            TouchImage(image);
        }
        return image;
    }

    /// sotc: the image with this id if it is still the registered image `uid`, else nullptr.
    [[nodiscard]] Image* TryGetImage(ImageId id, u64 uid) {
        if (!id || !slot_images.is_allocated(id)) {
            return nullptr;
        }
        auto& image = slot_images[id];
        if (image.image_uid != uid || !(image.flags & ImageFlagBits::Registered)) {
            return nullptr;
        }
        return &image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// sotc: IsMetaCleared + TouchMeta(address, slice, false) with a single lookup
    /// (SOTC_RT_FAST): returns the old clear bit of the slice (false when the address is not a
    /// metadata surface, like IsMetaCleared) and clears it.
    bool TestAndTouchMeta(VAddr address, u32 slice) {
        auto it = surface_metas.find(address);
        if (it == surface_metas.end()) {
            return false;
        }
        auto& clear_mask = it.value().clear_mask;
        const bool was_clear = (clear_mask & (1u << slice)) != 0;
        clear_mask &= ~(1u << slice);
        return was_clear;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            meta_epoch.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_RS_MEMO
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
                meta_epoch.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_RS_MEMO
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    /// sotc: SOTC_HRR dry run (buffer_cache/hrr.h): whether any image overlaps the range.
    [[nodiscard]] bool HasImageInRange(VAddr address, size_t size) {
        // sotc: ForEachImageInRegion flags images (Picked) and reads the page table, which a guest
        // unmap changes under `mutex`: lock like the other lookups (reentrant when already held).
        std::unique_lock lk{mutex, std::defer_lock};
        LockCounted(lk);
        bool found = false;
        ForEachImageInRegion(address, size, [&found](ImageId, Image&) {
            found = true;
            return true;
        });
        return found;
    }

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PAGE_BITS;
        for (u64 page = addr >> Traits::PAGE_BITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

public:
    /// Diagnostic (SOTC_DUMP): copies every layer/slice of mip 0 to a raw file once the GPU is done.
    void DumpImage(ImageId image_id, const std::filesystem::path& path);

    /// sotc: SOTC_TEX_DUMP=1 saves each small sampled texture once (user/texdump), keyed by the
    /// hash of its guest bytes, to find the game's controller glyphs.
    void SotcGlyphHook(Image& image);

private:

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id, ImageLeave why) {
        if (SotcStats::kFpsLog) { // sotc: B4, the image leaves the cache
            image_frees[static_cast<size_t>(why)].fetch_add(1, std::memory_order_relaxed);
        }
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    /// sotc: depth/colour twins (ResolveDepthOverlap) and unmapped images parked by SOTC_UNMAP_PARK.
    /// Park: what FreeImage does except the reclaim of the slot and its Vulkan objects. Unpark:
    /// registers it again as if new.
    void ParkImage(ImageId image_id, ImageLeave why);
    void UnparkImage(ImageId image_id);
    void ReclaimImage(ImageId image_id);

    /// sotc: B4 counters (SOTC_FPS), read and reset by ReportImageFrees.
    static inline std::array<std::atomic<u64>, NumImageLeave> image_frees{};
    static inline std::array<std::atomic<u64>, NumImageLeave> image_parks{};
    static inline std::atomic<u64> image_registers{};
    /// sotc: C5 counters (SOTC_UNMAP_PARK), read and reset by ReportUnmapPark. The two gauges are
    /// the parked set as of its last change.
    static inline std::atomic<u64> unmap_parks{}, unmap_rearms{}, unmap_evicts{};
    static inline std::atomic<u64> unmap_stale_avoided{}, unmap_would_park{}, unmap_would_rearm{};
    static inline std::atomic<u64> unmap_parked_now{}, unmap_parked_now_bytes{};
    /// sotc: C5 (SOTC_UNMAP_PARK, cache lock held). ParkUnmapped parks an image UnmapMemory is
    /// about to free (mode 1; mode 2 only remembers its shape and returns false). TakeUnmapParked
    /// re-arms the parked image of `info`'s address and shape, or returns an empty id.
    /// EvictOldestUnmapParked reclaims the oldest; DropUnmapParked all (EmergencyCollect).
    bool ParkUnmapped(ImageId image_id, u32 park_mode);
    ImageId TakeUnmapParked(const ImageInfo& info);
    void EvictOldestUnmapParked();
    void DropUnmapParked();
    void SyncUnmapParkGauges();
    void ShadowNoteUnmapped(const ImageInfo& info);
    void ShadowTake(const ImageInfo& info);

    void GarbageCollectImages();

    /// sotc: image memory by class, every 5 s with SOTC_FPS (telemetry only).
    void Census();
    /// sotc: SOTC_POOL_WATCH=1: logs game page pool pages freed under images still in use.
    void PoolWatch();
    static bool PoolWatchEnabled();

    /// sotc: mip-tail images (SOTC_MIP_TAIL), see DecideRebase.
    bool IsSafeToDelete(const Image& image) const;
    bool DecideRebase(ImageDesc& desc, bool exact_fmt);
    void MipTailCreated(ImageId image_id, const ImageDesc& desc);
    void FreeRebased(ImageId image_id, std::atomic<std::uint64_t>& counter);
    /// First GPU write of a target or storage image (FindTexture/RenderTarget/DepthTarget).
    void MarkWritten(Image& image);
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    BindlessSamplers bindless_samplers{instance}; // sotc: SOTC_BINDLESS_SAMP heap (samplers_mutex)
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    const bool readback_linear_images;
    PageTable page_table;
    /// sotc: images registered per page_table page (1 MiB), read without the lock by
    /// InvalidateMemory (guest write faults).
    static constexpr u64 NumImagePages = u64{1} << (Traits::ADDRESS_SPACE_BITS - Traits::PAGE_BITS);
    std::unique_ptr<std::atomic<u32>[]> page_images{new std::atomic<u32>[NumImagePages]()};
    ReentrantMutex mutex;
    /// sotc: RefreshImage nesting on this thread (it runs under `mutex`).
    static inline thread_local u32 refresh_depth = 0;
    /// sotc: deferred EmergencyCollect level + 1 (0: none).
    std::atomic<u32> pending_emergency{0};
    /// sotc: FindImage's perfect matches, valid while no image is registered or unregistered
    /// (a mip-tail image's also while gpu_modified_epoch stays). `writer`: stored by a writer
    /// lookup (with mip tails on, writers only take those).
    struct FindMemo {
        VAddr guest_address{};
        u64 guest_size{};
        Extent3D size{};
        vk::Format pixel_format{};
        AmdGpu::ImageType type{};
        bool exact_fmt{};
        bool writer{};
        u64 generation{~0ULL};
        ImageId image_id{};
        u64 gpu_epoch{};
        u64 image_uid{}; ///< sotc: the image the slot held when memoized (slots are reused)
    };
    /// sotc: direct-mapped: a second key on a slot evicts the first.
    static constexpr size_t NumFindMemos = 1024;
    std::unique_ptr<std::array<FindMemo, NumFindMemos>> find_memos =
        std::make_unique<std::array<FindMemo, NumFindMemos>>();
    FindMemo& MemoSlot(const ImageInfo& info, bool exact_fmt, bool writer);
    /// The memoized perfect match for a lookup, or none (SOTC_FIND_MEMO=0: always none).
    ImageId MemoLookup(const ImageInfo& info, bool exact_fmt, bool writer);
    /// The generation a memo for `address` is valid in (per-address unless SOTC_MEMO_ADDR_GEN=0).
    u64 MemoGeneration(VAddr address) const;
    /// sotc: SOTC_GEN_TEX identity memo (FindTextureIdentity), direct-mapped on the key. Allocated
    /// only when TexIdentityMode() is on.
    struct TexIdMemo {
        TexIdentityKey key{};
        bool exact_fmt{};
        bool valid{};
        ImageId image_id{};
        u64 image_uid{}; ///< the image the slot held when stored (slots are reused)
        ImageViewInfo pre_view_info{}; ///< desc.view_info as the caller built it (before FindImage);
                                       ///< the caller may adjust it per binding (mip fallback index)
        ImageViewInfo view_info{};     ///< desc.view_info after FindImage: the view it resolves to
        u64 epoch{};               ///< tex_epoch when stored
        u64 sum{};                 ///< SotcGen::Sum of the image's guest range when stored
    };
    static constexpr size_t NumTexIdMemos = 4096;
    std::unique_ptr<std::array<TexIdMemo, NumTexIdMemos>> tex_id_memos;

    /// sotc: SOTC_RT_MAXEXTENT family of a render or depth target: everything that fixes its
    /// layout except the layer count. Pitch and height fix BlockDim, so the overlap rules treat
    /// two requests of one family as one image (a view or an expand), never as a free.
    struct RtFamily {
        VAddr address{};
        VAddr stencil_addr{};
        u32 stencil_size{};
        u32 pitch{};
        u32 width{};
        u32 height{};
        u32 depth{};
        u32 levels{};
        u32 num_bits{};
        u32 num_samples{};
        u32 flags{}; ///< is_volume, is_tiled, is_pow2, is_block, is_depth, has_stencil (bits 0-5)
        vk::Format pixel_format{};
        AmdGpu::ImageType type{};
        AmdGpu::TileMode tile_mode{};
        AmdGpu::ArrayMode array_mode{};
        bool alt_tile{};
        bool operator==(const RtFamily&) const = default;
    };
    struct RtFamilyHash {
        size_t operator()(const RtFamily& f) const noexcept {
            u64 h = 0x9E3779B97F4A7C15ULL;
            const auto mix = [&h](u64 v) {
                h = (h ^ v) * 0xC2B2AE3D27D4EB4FULL;
                h ^= h >> 29;
            };
            mix(f.address);
            mix(f.stencil_addr);
            mix(f.stencil_size);
            mix(u64(f.pitch) << 32 | f.width);
            mix(u64(f.height) << 32 | f.depth);
            mix(u64(f.levels) << 32 | f.num_bits);
            mix(u64(f.num_samples) << 32 | f.flags);
            mix(static_cast<u64>(f.pixel_format));
            mix(static_cast<u64>(f.type));
            mix(static_cast<u64>(f.tile_mode) << 8 | static_cast<u64>(f.array_mode));
            mix(f.alt_tile);
            return static_cast<size_t>(h ^ (h >> 32));
        }
    };
    struct RtExtentEntry {
        u32 max_layers{};   ///< layers new images of the family are created with
        u32 first_layers{}; ///< layers the old path's image of the family would have
        ImageId image_id{}; ///< the family's latest image (valid while image_uid matches)
        u64 image_uid{};
    };
    /// The family of a render or depth target request.
    [[nodiscard]] static RtFamily RtFamilyOf(const ImageInfo& info);
    static constexpr size_t MaxRtFamilies = 4096;
    std::unordered_map<RtFamily, RtExtentEntry, RtFamilyHash> rt_extents;
    /// sotc: moves when an image is (un)registered or its ImageInfo is replaced (aliasing, mip
    /// tails, unmap parking): every identity memo entry older than the move is stale. Under `mutex`.
    u64 tex_epoch{};
    u64 image_set_generation{};
    /// sotc: SOTC_BIND_CACHE_FRAME: (un)registered image ranges (ImageChangedSince).
    VideoCore::RangeStamps image_stamps;
    /// sotc: per-address generations for FindMemo. A perfect match needs the same
    /// guest_address, so only (un)registering an image at that address can change the
    /// result; bumped in RegisterImage/UnregisterImage at the image's guest_address.
    static constexpr size_t NumAddrGens = 4096;
    std::array<u32, NumAddrGens> addr_gens{};
    static size_t AddrGenSlot(VAddr addr) {
        return static_cast<size_t>((u64{addr >> 8} * 0x9E3779B97F4A7C15ULL) >> 52);
    }
    std::mutex samplers_mutex;
    // sotc: bumped whenever a sampler is erased (GetSampler's per-thread memo).
    std::atomic<u64> sampler_epoch{0};
    std::mutex download_images_mutex;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
    std::atomic<u64> meta_epoch{0}; ///< sotc: MetaEpoch()
    /// sotc: SOTC_ASYNC_IMAGE_CREATE: the T#s PrefetchTexture saw lately (command processor
    /// thread only), by a hash of the T# words: when it last looked and last asked for an image.
    struct PrefetchSeen {
        u64 tag{};
        u64 checked_ns{};
        u64 requested_ns{};
        u32 misses{}; ///< sotc: requests not followed by the image (each doubles the wait)
    };
    static constexpr size_t NumPrefetchSeen = 4096;
    std::unique_ptr<std::array<PrefetchSeen, NumPrefetchSeen>> prefetch_seen;
    /// sotc: SOTC_ASYNC_IMAGE_CREATE (null when off). Declared last: destroyed first, so its
    /// workers stop and the images nobody took are freed while everything else still exists.
    std::unique_ptr<ImagePrefetcher> image_prefetcher;
    /// sotc: SOTC_AREA_PREWARM=3 replay of the current area (command processor thread only): its
    /// records, the next one to process, and the guest bytes accepted so far (the area's budget).
    std::vector<SotcAreaPrewarm::ImageRecord> replay_records;
    size_t replay_next{};
    u64 replay_accepted{};
    /// sotc: SOTC_UNMAP_PARK (C5): the parked images, oldest first (unregistered, Vulkan objects
    /// kept; `bytes` is their VRAM), and the shapes mode 2 would have parked.
    struct UnmapParked {
        ImageId id{};
        u64 bytes{};
        u64 tick{}; // gc_tick at the park
    };
    std::deque<UnmapParked> unmap_parked;
    u64 unmap_parked_bytes{};
    std::deque<ImageInfo> unmap_shadow;
};

} // namespace VideoCore
