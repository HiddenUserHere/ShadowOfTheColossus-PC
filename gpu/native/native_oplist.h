// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. OpList: the compact form of an all-native command buffer for the
// native command processor (W3, SOTC_NATIVE_CP). Built once at submit time from the guest buffer
// (every packet must be a NOP; native_stream_ops.h payloads become items, every other NOP is
// skipped), executed by Liverpool::RunNativeEntry (liverpool_native_cp.cpp) without parsing PM4.
#pragma once

#include <span>
#include <vector>

#include "common/types.h"

namespace Native {

enum class OpKind : u8 {
    Op,     ///< SotcNativeOp: `id` (flag bits kept: bit 31 writes memory, bit 30 run on CP)
    Wait,   ///< SotcNativeWait: `id` of the queued predicate (StreamOps::Poll)
    WaitCe, ///< SotcNativeWaitCe: `id`; the DE side resumes the CE cursor until it holds
    Ib,     ///< SotcNativeIb: nested buffer [ib_addr, ib_addr + ib_size * 4), built lazily
};

struct OpItem {
    OpKind kind{};
    u32 id{};             ///< Op / Wait / WaitCe: the id dword as written in the NOP
    u64 ib_addr{};        ///< Ib only
    u32 ib_size{};        ///< Ib only, dwords
    const u32* body{};    ///< the NOP body after the id (prefetch shadow when tagged), or null
    u32 body_dwords{};    ///< dwords at `body`
    u32 dw_end{};         ///< offset (dwords) just past this packet in the source buffer
};

struct OpList {
    std::vector<OpItem> items;
    u32 dwords{}; ///< size of the source buffer in dwords (compute read-pointer advance)
    void Clear() {
        items.clear();
        dwords = 0;
    }
};

/// Builds `out` from `cb`. Returns false (and leaves `out` unspecified) when any packet is not a
/// NOP, a packet is truncated by the end of `cb` (a compute ring slice split mid-packet), or the
/// buffer is empty. Type-2 filler and the one-dword NOP 0xffff1000 are skipped. Thread-safe (no
/// shared state); called on the submitting guest thread.
bool BuildOpList(std::span<const u32> cb, OpList& out);

/// Stats: per OpKind counts of the last BuildOpList calls (cheap relaxed counters, for the
/// SOTC NATIVE STATS line).
void CountOpList(const OpList& list);

} // namespace Native
