// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_MEM (S1): the guest page table for every buffer kind, filled by events (gcn_native.h, namespace Mem).
// The table itself is SotcArch::AddrTable (addr_table.h); this file holds the stage's entry points, its verify and its
// counters.
#include <atomic>
#include <cstdio>

#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/gcn_native.h"

namespace SotcGcn::Mem {

namespace {

constexpr u64 kMaxLoggedMismatches = 16;

std::atomic<u64> g_imports{};          // ring-8 runs placed in the table (OnImport calls that placed pages)
std::atomic<u64> g_import_bytes{};     // whole table pages of those runs
std::atomic<u64> g_releases{};         // OnRelease calls
std::atomic<u64> g_placed_checks{};    // Placed calls
std::atomic<u64> g_placed_hits{};      // Placed calls whose range was entirely placed
std::atomic<u64> g_verify_bindings{};  // mode 2: classic bindings compared
std::atomic<u64> g_verify_match{};     // ... whose table answer equals ObtainBuffer's
std::atomic<u64> g_verify_differ{};    // ... whose table answer differs (logged, first 16)
std::atomic<u64> g_verify_absent{};    // ... whose table answer is 0 (page not in the table)
std::atomic<u64> g_verify_logged{};    // mismatches seen, for the log limit (not reset by Report)

} // namespace

bool On() {
    return Mode(Stage::Mem) != 0;
}

bool Verify() {
    return Mode(Stage::Mem) == 2;
}

void VerifyBinding(VAddr addr, u64 size, const VideoCore::Buffer* buffer, u64 offset, bool is_written) {
    if (!Verify() || size == 0 || !buffer || !buffer->HasBufferDeviceAddress()) {
        return;
    }
    // The device address of the first and of the last byte of the range, as ObtainBuffer's answer places them.
    const vk::DeviceAddress expect_first = buffer->BufferDeviceAddress() + offset;
    const vk::DeviceAddress expect_last = expect_first + (size - 1);
    const vk::DeviceAddress table_first = SotcArch::AddrTable::Translate(addr);
    const vk::DeviceAddress table_last = SotcArch::AddrTable::Translate(addr + size - 1);
    g_verify_bindings.fetch_add(1, std::memory_order_relaxed);
    if (table_first == 0 || table_last == 0) {
        g_verify_absent.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (table_first == expect_first && table_last == expect_last) {
        g_verify_match.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_verify_differ.fetch_add(1, std::memory_order_relaxed);
    if (g_verify_logged.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedMismatches) {
        LOG_WARNING(Render,
                    "SOTC GCN MEM verify: addr {:#x} size {:#x}, table {:#x} / {:#x}, buffer {:#x} / {:#x}, "
                    "written {}",
                    addr, size, table_first, table_last, expect_first, expect_last, is_written);
    }
}

bool Placed(VAddr addr, u64 size) {
    if (!On()) {
        return false;
    }
    g_placed_checks.fetch_add(1, std::memory_order_relaxed);
    const bool all = SotcArch::AddrTable::AllPlaced(addr, size);
    if (all) {
        g_placed_hits.fetch_add(1, std::memory_order_relaxed);
    }
    return all;
}

void OnImport(VAddr addr, u64 size, vk::DeviceAddress device_addr, const VideoCore::Buffer* owner) {
    if (!On() || size == 0 || device_addr == 0) {
        // No device address (the run has no BDA): its pages stay unplaced and Touch serves them.
        return;
    }
    // Whole table pages only. A page shared with a neighbouring run (the first and last page of a run that starts or
    // ends inside a page) stays unplaced; its Touch goes through ObtainBuffer.
    constexpr u64 kPage = SotcArch::AddrTable::PageSize;
    const VAddr page_lo = (addr + kPage - 1) & ~(kPage - 1);
    const VAddr page_hi = (addr + size) & ~(kPage - 1);
    if (page_hi <= page_lo) {
        return;
    }
    g_imports.fetch_add(1, std::memory_order_relaxed);
    g_import_bytes.fetch_add(page_hi - page_lo, std::memory_order_relaxed);
    // owner is nullptr for a ring-8 run (it is a vk::Buffer of the ring, not a VideoCore::Buffer): Backing refuses
    // such pages, so ObtainRead falls back to ObtainBuffer for them.
    SotcArch::AddrTable::OnPlace(page_lo, page_hi - page_lo, device_addr + (page_lo - addr), owner);
}

void OnRelease(VAddr addr, u64 size) {
    if (!On() || size == 0) {
        return;
    }
    g_releases.fetch_add(1, std::memory_order_relaxed);
    SotcArch::AddrTable::OnRemove(addr, size);
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    if (!On()) {
        return;
    }
    const u64 imports = g_imports.exchange(0, std::memory_order_relaxed);
    const u64 import_bytes = g_import_bytes.exchange(0, std::memory_order_relaxed);
    const u64 releases = g_releases.exchange(0, std::memory_order_relaxed);
    const u64 checks = g_placed_checks.exchange(0, std::memory_order_relaxed);
    const u64 hits = g_placed_hits.exchange(0, std::memory_order_relaxed);
    const u64 bindings = g_verify_bindings.exchange(0, std::memory_order_relaxed);
    const u64 match = g_verify_match.exchange(0, std::memory_order_relaxed);
    const u64 differ = g_verify_differ.exchange(0, std::memory_order_relaxed);
    const u64 absent = g_verify_absent.exchange(0, std::memory_order_relaxed);
    int n = std::snprintf(out, size,
                          "SOTC GCN MEM: runs placed %llu (%llu KiB), released %llu, placed checks %llu (all placed "
                          "%llu)",
                          static_cast<unsigned long long>(imports),
                          static_cast<unsigned long long>(import_bytes >> 10),
                          static_cast<unsigned long long>(releases), static_cast<unsigned long long>(checks),
                          static_cast<unsigned long long>(hits));
    if (n < 0 || static_cast<u32>(n) >= size) {
        return;
    }
    if (Verify()) {
        const int m = std::snprintf(out + n, size - static_cast<u32>(n),
                                    ", verify bindings %llu (match %llu, differ %llu, not in table %llu)",
                                    static_cast<unsigned long long>(bindings),
                                    static_cast<unsigned long long>(match),
                                    static_cast<unsigned long long>(differ),
                                    static_cast<unsigned long long>(absent));
        if (m > 0) {
            n += m;
        }
    }
    if (static_cast<u32>(n) < size) {
        std::snprintf(out + n, size - static_cast<u32>(n), " over %.1f s", seconds);
    }
}

} // namespace SotcGcn::Mem
