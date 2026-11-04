// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_SYNC (S4, gcn_native.h, docs/re-stutters/native/GCN-NATIVE-DECISIONS.md D10). Freshness per submit:
// instead of every table-addressed buffer bind checking its own range, one batch at the start of the submit
// synchronizes every placed table page that the CPU changed, and refreshes every image the T# table references
// whose guest memory the CPU changed.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/gcn_native.h"
#include "video_core/texture_cache/texture_cache.h"
#include "../../../sotc/pc_ring.h" // SotcPcRing::HostServed

namespace SotcGcn::Sync {

namespace {

std::atomic<u64> g_host_served{}; // pieces served in place (ring-8 imports), no upload

// One buffer cache call per piece: ObtainBuffer takes a u32 size, and the table rejects ranges above 4 GiB.
// 64 MiB is the cap the other early-sync paths use (BufferCache::WarmRange, DrainRange).
constexpr u64 kMaxPieceBytes = u64{64} << 20;
constexpr u64 kLoggedStale = 16;

std::atomic<u64> g_batches{};     // SubmitBatch calls with the mode on
std::atomic<u64> g_runs{};        // table runs synchronized by the batch
std::atomic<u64> g_run_bytes{};   // bytes of those runs
std::atomic<u64> g_images{};      // table images refreshed by the batch (CPU-dirty)
std::atomic<u64> g_checks{};      // mode 2: per-op freshness checks compared
std::atomic<u64> g_stale{};       // mode 2: of those, the per-op path found stale data the batch should have taken
std::atomic<u64> g_logged{};      // stale lines logged (first kLoggedStale only)

struct StaleRun {
    VAddr addr;
    u64 size;
};

void CollectStaleRun(void* user, VAddr addr, u64 size) {
    static_cast<std::vector<StaleRun>*>(user)->push_back({addr, size});
}

struct ImageContext {
    VideoCore::TextureCache* texture_cache;
    u64 refreshed;
};

void RefreshImageCallback(void* user, VideoCore::Image& image) {
    auto* ctx = static_cast<ImageContext*>(user);
    if (ctx->texture_cache->SotcRefreshCpuDirty(image)) {
        ++ctx->refreshed;
    }
}

} // namespace

bool On() {
    return Mode(Stage::Sync) != 0;
}

bool Verify() {
    return Mode(Stage::Sync) == 2;
}

bool SkipOpSync() {
    return Mode(Stage::Sync) == 1;
}

void SubmitBatch(VideoCore::BufferCache& cache, VideoCore::TextureCache& texture_cache,
                 Vulkan::Scheduler& scheduler) {
    if (!On()) {
        return;
    }
    (void)scheduler; // the sync runs on the upload command buffer through the buffer cache; nothing is recorded here
    g_batches.fetch_add(1, std::memory_order_relaxed);

    // (1) Buffers. The stale runs are collected first, so the table's lock is not held while the buffer cache runs.
    // Per piece, as the read path does (AddrTable::Touch): the clean sequence is read before the sync, the sync is
    // ObtainBuffer (the same call, on the upload command buffer), and the bits are set only when no CPU change
    // was left unsynced and no clean state changed since the sequence (MarkClean).
    thread_local std::vector<StaleRun> runs;
    runs.clear();
    SotcArch::AddrTable::ForEachStalePlaced(CollectStaleRun, &runs);
    for (const StaleRun& run : runs) {
        for (u64 done = 0; done < run.size;) {
            const u64 piece = std::min(run.size - done, kMaxPieceBytes);
            const VAddr piece_addr = run.addr + done;
            done += piece;
            const u64 sequence = SotcArch::AddrTable::CleanSequence();
            // sotc: SOTC_GCN_SYNC. A piece inside one imported ring-8 run is read by the GPU in place (zero copy): no
            // upload is needed, and ObtainBuffer would build arena blocks over it and stamp them all as used.
            if (SotcPcRing::HostServed(piece_addr, piece)) {
                SotcArch::AddrTable::MarkClean(piece_addr, piece, sequence);
                g_host_served.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto [buffer, offset] = cache.ObtainBuffer(piece_addr, static_cast<u32>(piece), false);
            if (!buffer || !buffer->HasBufferDeviceAddress()) {
                continue; // refused: the pages stay stale and the next submit (or a bind) takes them
            }
            // The table must name the bytes this sync wrote (Touch writes its entries from the same answer; the batch
            // does not, so it checks). A mismatch leaves the pages stale for a bind's Touch.
            const vk::DeviceAddress device = buffer->BufferDeviceAddress() + offset;
            if (SotcArch::AddrTable::Translate(piece_addr) != device ||
                SotcArch::AddrTable::Translate(piece_addr + piece - 1) != device + piece - 1) {
                continue;
            }
            if (cache.IsRegionCpuModified(piece_addr, piece)) {
                continue; // a CPU change the sync did not take: the bits stay clear
            }
            if (SotcArch::AddrTable::MarkClean(piece_addr, piece, sequence)) {
                g_runs.fetch_add(1, std::memory_order_relaxed);
                g_run_bytes.fetch_add(piece, std::memory_order_relaxed);
            }
        }
    }

    // (2) Images the T# table references, whose guest memory the CPU changed.
    if (Tex::On()) {
        ImageContext ctx{&texture_cache, 0};
        Tex::ForEachReferencedImage(texture_cache, RefreshImageCallback, &ctx);
        g_images.fetch_add(ctx.refreshed, std::memory_order_relaxed);
    }
}

void NoteOpSync(VAddr addr, u64 size, bool found_stale) {
    if (!Verify()) {
        return;
    }
    g_checks.fetch_add(1, std::memory_order_relaxed);
    if (!found_stale) {
        return;
    }
    g_stale.fetch_add(1, std::memory_order_relaxed);
    if (g_logged.fetch_add(1, std::memory_order_relaxed) < kLoggedStale) {
        LOG_WARNING(Render_Vulkan, "SOTC GCN SYNC: per-op path found stale guest bytes at {:#x}+{:#x}, the batch missed them",
                    addr, size);
    }
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    if (!On()) {
        out[0] = '\0';
        return;
    }
    const u64 batches = g_batches.exchange(0, std::memory_order_relaxed);
    const u64 runs = g_runs.exchange(0, std::memory_order_relaxed);
    const u64 run_bytes = g_run_bytes.exchange(0, std::memory_order_relaxed);
    const u64 images = g_images.exchange(0, std::memory_order_relaxed);
    const u64 checks = g_checks.exchange(0, std::memory_order_relaxed);
    const u64 stale = g_stale.exchange(0, std::memory_order_relaxed);
    const double per_s = seconds > 0.0 ? 1.0 / seconds : 0.0;
    std::snprintf(out, size,
                  "SOTC GCN SYNC: batches %.1f/s, runs %.0f/s (%.1f MiB/s), in place %.0f/s, images %.1f/s, "
                  "verify checks %llu stale %llu",
                  static_cast<double>(batches) * per_s, static_cast<double>(runs) * per_s,
                  static_cast<double>(run_bytes) * per_s / static_cast<double>(1ULL << 20),
                  static_cast<double>(g_host_served.exchange(0, std::memory_order_relaxed)) * per_s,
                  static_cast<double>(images) * per_s, static_cast<unsigned long long>(checks),
                  static_cast<unsigned long long>(stale));
}

} // namespace SotcGcn::Sync
