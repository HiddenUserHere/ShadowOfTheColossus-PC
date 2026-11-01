// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RT_CLASS, render targets in RTAllocator blocks. See rt_class.h.

#include "rt_class.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace SotcRtClass {
namespace {

std::mutex g_mutex;
std::map<uint64_t, uint64_t> g_blocks; // base -> size, RTAllocator blocks only
std::atomic<size_t> g_block_count{0};  // lock-free "no blocks" test for Contains

std::atomic<uint64_t> g_downloads{0};
std::atomic<uint64_t> g_download_bytes{0};
std::atomic<uint64_t> g_downloads_skipped{0};
std::atomic<uint64_t> g_download_skipped_bytes{0};
std::atomic<uint64_t> g_tracks{0};
std::atomic<uint64_t> g_tracks_skipped{0};
std::atomic<uint64_t> g_cpu_invalidations{0};

} // namespace

int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_RT_CLASS");
        if (!v) {
            return 0;
        }
        const int m = std::atoi(v);
        return m == 1 || m == 2 ? m : 0;
    }();
    return mode;
}

void OnBlock(uint64_t base, uint64_t size, const char* name, int kind) {
    (void)kind;
    if (Mode() == 0 || !name || std::strcmp(name, "RTAllocator") != 0 || size == 0) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    g_blocks[base] = size;
    g_block_count.store(g_blocks.size(), std::memory_order_release);
}

void OnFreeBlock(uint64_t base) {
    if (Mode() == 0) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    g_blocks.erase(base);
    g_block_count.store(g_blocks.size(), std::memory_order_release);
}

bool Contains(uint64_t addr, uint64_t size) {
    if (size == 0 || g_block_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    auto it = g_blocks.upper_bound(addr);
    if (it == g_blocks.begin()) {
        return false;
    }
    --it; // the last block with base <= addr
    const uint64_t offset = addr - it->first;
    return offset <= it->second && size <= it->second - offset;
}

void NoteDownload(uint64_t bytes) {
    g_downloads.fetch_add(1, std::memory_order_relaxed);
    g_download_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteDownloadSkipped(uint64_t bytes) {
    g_downloads_skipped.fetch_add(1, std::memory_order_relaxed);
    g_download_skipped_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteTrack() {
    g_tracks.fetch_add(1, std::memory_order_relaxed);
}

void NoteTrackSkipped() {
    g_tracks_skipped.fetch_add(1, std::memory_order_relaxed);
}

void NoteCpuInvalidate() {
    g_cpu_invalidations.fetch_add(1, std::memory_order_relaxed);
}

void Report(std::string& out, double seconds) {
    const int mode = Mode();
    if (mode == 0) {
        return;
    }
    uint64_t block_bytes = 0;
    size_t blocks = 0;
    {
        std::scoped_lock lock{g_mutex};
        blocks = g_blocks.size();
        for (const auto& [base, size] : g_blocks) {
            block_bytes += size;
        }
    }
    const auto downloads = g_downloads.exchange(0, std::memory_order_relaxed);
    const auto download_bytes = g_download_bytes.exchange(0, std::memory_order_relaxed);
    const auto downloads_skipped = g_downloads_skipped.exchange(0, std::memory_order_relaxed);
    const auto skipped_bytes = g_download_skipped_bytes.exchange(0, std::memory_order_relaxed);
    const auto tracks = g_tracks.exchange(0, std::memory_order_relaxed);
    const auto tracks_skipped = g_tracks_skipped.exchange(0, std::memory_order_relaxed);
    const auto invalidations = g_cpu_invalidations.exchange(0, std::memory_order_relaxed);

    char line[512];
    std::snprintf(
        line, sizeof line,
        "SOTC RT CLASS: mode %d, %zu RTAllocator blocks (%llu MiB), over %.1f s: "
        "downloads %llu (%llu KiB), CPU tracks %llu, CPU write invalidations %llu (normal "
        "path); mode 1 skipped: downloads %llu (%llu KiB), tracks %llu\n",
        mode, blocks, static_cast<unsigned long long>(block_bytes >> 20), seconds,
        static_cast<unsigned long long>(downloads),
        static_cast<unsigned long long>(download_bytes >> 10),
        static_cast<unsigned long long>(tracks), static_cast<unsigned long long>(invalidations),
        static_cast<unsigned long long>(downloads_skipped),
        static_cast<unsigned long long>(skipped_bytes >> 10),
        static_cast<unsigned long long>(tracks_skipped));
    out += line;
}

} // namespace SotcRtClass
