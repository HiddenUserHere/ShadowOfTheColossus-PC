// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BDA groundwork, see vk_bda_table.h.

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_bda_table.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

namespace {

u32 EnvMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_BDA");
        if (!v || v[0] == '\0') {
            return 0u;
        }
        return v[0] == '2' ? 2u : (v[0] == '1' ? 1u : 0u);
    }();
    return mode;
}

struct Counters {
    std::atomic<u64> draws{};
    std::atomic<u64> slots{};
    std::atomic<u64> nulls{};
    std::atomic<u64> no_bda{};
    std::atomic<u64> misaligned{};
    std::atomic<u64> verify_fail{};
    std::atomic<u64> table_bytes{};
};
Counters g_counters;

} // namespace

u32 BdaTableMode(const Instance& instance) {
    return instance.IsShaderInt64Supported() ? EnvMode() : 0u;
}

void BdaTable::Begin(u32 num_slots, u64 null_addr) {
    null_address = null_addr;
    words.assign(static_cast<size_t>(num_slots) * 4, 0);
    for (u32 slot = 0; slot < num_slots; slot++) {
        Write(slot, null_address, 0);
    }
    g_counters.draws.fetch_add(1, std::memory_order_relaxed);
    g_counters.table_bytes.fetch_add(Bytes(), std::memory_order_relaxed);
}

void BdaTable::Write(u32 slot, u64 address, u32 size) {
    u32* w = words.data() + static_cast<size_t>(slot) * 4;
    w[0] = static_cast<u32>(address);
    w[1] = static_cast<u32>(address >> 32);
    w[2] = size;
    w[3] = 0;
}

void BdaTable::SetNull(u32 slot) {
    Write(slot, null_address, 0);
    g_counters.nulls.fetch_add(1, std::memory_order_relaxed);
}

void BdaTable::SetBlock(u32 slot, u64 device_address, u64 buffer_bytes, u64 offset, u64 size) {
    g_counters.slots.fetch_add(1, std::memory_order_relaxed);
    if (size == 0) {
        SetNull(slot);
        return;
    }
    if (device_address == 0) {
        g_counters.no_bda.fetch_add(1, std::memory_order_relaxed);
        SetNull(slot);
        return;
    }
    if (EnvMode() == 2 && offset + size > buffer_bytes) {
        g_counters.verify_fail.fetch_add(1, std::memory_order_relaxed);
        SetNull(slot);
        return;
    }
    if ((device_address & 3) != 0) {
        g_counters.misaligned.fetch_add(1, std::memory_order_relaxed);
    }
    // The block is the whole V# range; a size above 4 GiB cannot be a slot size.
    Write(slot, device_address + offset, static_cast<u32>(size > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : size));
}

void BdaTableReport(char* out, u32 out_size) {
    const u64 draws = g_counters.draws.exchange(0, std::memory_order_relaxed);
    const u64 slots = g_counters.slots.exchange(0, std::memory_order_relaxed);
    const u64 nulls = g_counters.nulls.exchange(0, std::memory_order_relaxed);
    const u64 no_bda = g_counters.no_bda.exchange(0, std::memory_order_relaxed);
    const u64 misaligned = g_counters.misaligned.exchange(0, std::memory_order_relaxed);
    const u64 verify_fail = g_counters.verify_fail.exchange(0, std::memory_order_relaxed);
    const u64 bytes = g_counters.table_bytes.exchange(0, std::memory_order_relaxed);
    std::snprintf(out, out_size,
                  "SOTC BDA draws %llu slots %llu nulls %llu no_bda %llu misaligned %llu "
                  "verify_fail %llu table %llu B",
                  static_cast<unsigned long long>(draws), static_cast<unsigned long long>(slots),
                  static_cast<unsigned long long>(nulls), static_cast<unsigned long long>(no_bda),
                  static_cast<unsigned long long>(misaligned),
                  static_cast<unsigned long long>(verify_fail),
                  static_cast<unsigned long long>(bytes));
}

} // namespace Vulkan
