// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: ARCH-GCN-NATIVE S2 host side (gcn_native.h, namespace SotcGcn::Buf). Every guest buffer a gcn pipeline reads or
// writes is found by address: the page table (AddrTable) is the answer, and a CPU bit test replaces the per-op
// residency work when every page is already placed (D4). Decisions D4 and D5 in GCN-NATIVE-DECISIONS.md.
#include <atomic>
#include <cstdio>

#include "common/types.h"
#include "shader_recompiler/info.h" // Shader::GenericBufferFormat (SOTC_GENERIC_BUFFERS=2)
#include "video_core/buffer_cache/buffer_cache.h" // NoteBindUse
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/gcn_native.h"
#include "video_core/sotc_arch/sotc_arch.h" // ShaderAddr::Mode

namespace SotcGcn::Buf {
namespace {

std::atomic<u64> g_fast{};          // binds answered by the placed-page test alone (mode 1)
std::atomic<u64> g_slow{};          // binds that ran AddrTable::Touch (mode 1: writes and unplaced or unsynced ranges)
std::atomic<u64> g_misses{};        // page misses drained from the miss ring and resolved (OnPageMiss)
std::atomic<u64> g_miss_failed{};   // page misses Touch could not make resident
std::atomic<u64> g_verify_eligible{}; // mode 2: reads the fast path would have skipped (placed, sync batch on)
std::atomic<u64> g_verify_moved{};  // mode 2: of those, Touch changed the table entries of the range (placement moved)

} // namespace

bool On() {
    return Mode(Stage::Buf) != 0;
}

bool Verify() {
    return Mode(Stage::Buf) == 2;
}

u8 ProfileValue() {
    if (!On()) {
        return 0;
    }
    // D5: the buffer path extends the table path (shader_addr 3 or 4), it does not replace it.
    const u8 shader_addr = SotcArch::ShaderAddr::Mode();
    if (shader_addr != 3 && shader_addr != 4) {
        Refuse(Stage::Buf, "SOTC_SHADER_ADDR must be in table mode (SOTC_ADDR_TABLE=1)");
        return 0;
    }
    // D5: the formatted buffers the path adds are decoded at run time, which only SOTC_GENERIC_BUFFERS=2 does.
    if (!Shader::GenericBufferFormat()) {
        Refuse(Stage::Buf, "SOTC_GENERIC_BUFFERS=2 is not set");
        return 0;
    }
    return static_cast<u8>(Mode(Stage::Buf));
}

bool BindAddressed(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr addr, u64 size,
                   bool is_written, bool is_texel) {
    if (size == 0) {
        return true;
    }
    if (!On()) {
        return SotcArch::AddrTable::Touch(cache, scheduler, addr, size, is_written, is_texel);
    }
    if (Verify()) {
        // Mode 2: Touch always runs, so the answer is the old path's. The fast path's predicate is evaluated first
        // and the entries are compared before and after (Translate of the first and last byte).
        // Limit: Touch does not report whether it synchronized guest bytes; the sync batch (S4) owns that, and a
        // sync-only Touch is not counted as moved.
        const bool eligible = !is_written && !is_texel && Mem::Placed(addr, size) && Sync::SkipOpSync();
        vk::DeviceAddress first_before{};
        vk::DeviceAddress last_before{};
        if (eligible) {
            first_before = SotcArch::AddrTable::Translate(addr);
            last_before = SotcArch::AddrTable::Translate(addr + size - 1);
        }
        const bool ok = SotcArch::AddrTable::Touch(cache, scheduler, addr, size, is_written, is_texel);
        g_slow.fetch_add(1, std::memory_order_relaxed);
        if (eligible) {
            g_verify_eligible.fetch_add(1, std::memory_order_relaxed);
            if (SotcArch::AddrTable::Translate(addr) != first_before ||
                SotcArch::AddrTable::Translate(addr + size - 1) != last_before) {
                g_verify_moved.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return ok;
    }

    // Mode 1. A read of placed pages whose freshness the submit batch owns is answered by the bit test alone.
    // sotc: a formatted buffer is never answered by the bit test: its read needs ObtainBuffer's image sync.
    if (!is_written && !is_texel && Mem::Placed(addr, size) && Sync::SkipOpSync()) {
        // The use stamp keeps the blocks out of the idle eviction, as Touch's own fast path does (addr_table.cpp):
        // an evicted block would read as zeros until the miss ring brings it back (D4 forbids that frame).
        cache.NoteBindUse(addr, size);
        g_fast.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Written ranges always go through Touch. The GPU-modified marking of a range (what makes a later written bind
    // fast in native mode) is made by ObtainBuffer with is_written, inside Touch; Touch's own fast path already
    // requires WrittenAlreadyGpuModified, so it is safe to call it for every written bind.
    g_slow.fetch_add(1, std::memory_order_relaxed);
    return SotcArch::AddrTable::Touch(cache, scheduler, addr, size, is_written, is_texel);
}

void OnPageMiss(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr page_addr) {
    const bool ok = SotcArch::AddrTable::Touch(cache, scheduler, page_addr, SotcArch::AddrTable::PageSize, false);
    g_misses.fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
        g_miss_failed.fetch_add(1, std::memory_order_relaxed);
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
    const u64 fast = g_fast.exchange(0, std::memory_order_relaxed);
    const u64 slow = g_slow.exchange(0, std::memory_order_relaxed);
    const u64 misses = g_misses.exchange(0, std::memory_order_relaxed);
    const u64 failed = g_miss_failed.exchange(0, std::memory_order_relaxed);
    const u64 eligible = g_verify_eligible.exchange(0, std::memory_order_relaxed);
    const u64 moved = g_verify_moved.exchange(0, std::memory_order_relaxed);
    const double per = seconds > 0.0 ? 1.0 / seconds : 0.0;
    const int n = std::snprintf(out, size,
                                "SOTC GCN BUF: binds fast %.0f/s slow %.0f/s, page misses %.1f/s (failed %llu)",
                                fast * per, slow * per, misses * per, static_cast<unsigned long long>(failed));
    if (Verify() && n >= 0 && static_cast<u32>(n) < size) {
        std::snprintf(out + n, size - static_cast<u32>(n),
                      ", verify: fast would skip %.0f/s, entries moved %.0f/s",
                      eligible * per, moved * per);
    }
}

} // namespace SotcGcn::Buf
