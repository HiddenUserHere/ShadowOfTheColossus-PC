// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. See native_stream_ops.h.

#include "native/native_stream_ops.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace Native::StreamOps {

namespace {

/// PM4CmdNop::PayloadType::SotcNativeOp (pm4_cmds.h); repeated here so this module does not pull
/// the PM4 headers.
constexpr u32 kPayload = 0x68750f01u;
/// Type-3 header: bits 30-31 = 3, count (body dwords - 1) in bits 16-29, opcode NOP (0x10) in
/// bits 8-15. Body = payload + id = 2 dwords -> count 1. Same encoding the guest's 0x86d210 uses
/// for its label NOPs (`(n) << 16 | 0xc0001000`).
constexpr u32 kHeader = 0xc0001000u | (1u << 16);
constexpr u32 kPacketDwords = 3;
/// Ops never reached by the command processor (a command buffer the game drops) would otherwise
/// accumulate; past this many pending ones the oldest ids are simply never run again.
constexpr std::size_t kMaxPending = 1u << 16;

std::mutex g_mutex;
std::unordered_map<u32, Common::UniqueFunction<void>> g_ops;
std::atomic<u32> g_next_id{1};
std::atomic<u32> g_missing{0};

/// The guest's refill callback (`(**(code **)(cb + 0x18))(cb, dwords, *(cb + 0x20))`, returns a
/// bool in al), guest SysV.
using RefillFn = u8(PS4_SYSV_ABI*)(u64 cb, u32 dwords, u64 arg);

bool WritePacket(u64 cb, const u32* dwords, u32 count) {
    if (cb == 0) {
        return false;
    }
    auto** cursor_slot = reinterpret_cast<u32**>(cb + 0x10);
    u32* cursor = *cursor_slot;
    const u32* end = *reinterpret_cast<u32* const*>(cb + 0x08);
    if (cursor == nullptr || end < cursor || static_cast<u64>(end - cursor) < count) {
        const auto refill = *reinterpret_cast<const RefillFn*>(cb + 0x18);
        if (refill == nullptr || refill(cb, count, *reinterpret_cast<const u64*>(cb + 0x20)) == 0) {
            return false;
        }
        cursor = *cursor_slot;
        end = *reinterpret_cast<u32* const*>(cb + 0x08);
        if (cursor == nullptr || end < cursor || static_cast<u64>(end - cursor) < count) {
            return false;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        cursor[i] = dwords[i];
    }
    *cursor_slot = cursor + count;
    return true;
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("SOTC_NATIVE_STREAM_OPS");
        return !e || e[0] != '0';
    }();
    return enabled;
}

bool Emit(u64 cb, Common::UniqueFunction<void>&& op) {
    u32 id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) { // wrapped: 0 is never handed out
        id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::scoped_lock lock{g_mutex};
        if (g_ops.size() >= kMaxPending) {
            return false;
        }
        g_ops.emplace(id, std::move(op));
    }
    const u32 packet[kPacketDwords] = {kHeader, kPayload, id};
    if (!WritePacket(cb, packet, kPacketDwords)) {
        std::scoped_lock lock{g_mutex};
        g_ops.erase(id);
        return false;
    }
    return true;
}

void Run(u32 id) {
    Common::UniqueFunction<void> op;
    {
        std::scoped_lock lock{g_mutex};
        const auto it = g_ops.find(id);
        if (it == g_ops.end()) {
            if (g_missing.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::fprintf(stderr, "SOTC RENDER native: stream op %u not queued (skipped)\n", id);
            }
            return;
        }
        op = std::move(it->second);
        g_ops.erase(it);
    }
    op();
}

} // namespace Native::StreamOps
