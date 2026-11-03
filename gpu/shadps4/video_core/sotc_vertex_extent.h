// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: vertex buffer binds narrowed to the bytes a direct draw fetches (SOTC_VERTEX_EXTENT=1, off by default).
//
// A vertex V# often says "unbounded" (num_records 0xffffffff). BindVertexBuffers then binds stride * num_records bytes
// from the base, and the bind syncs every CPU-dirty page of that range on the draw thread (6.7 MiB binds for V#s
// 0xc5b77ac8 and 0x6ca60583, docs/re-backend/15-burst-stutters.md). A draw fetches a few vertices of it.
//
// With the switch on, the SIZE of a vertex binding shrinks to the end of the last byte the draw can fetch. The BASE
// does not change, so every fetch address is the one it was before. The vertex input of the port (see
// GraphicsPipeline::GetVertexInputs) gives each guest attribute its own binding at attribute offset 0, and an element
// is at most kMaxElemBytes wide (the widest buffer format of SurfaceFormats() is 4 x 32 bits). So:
//   - vertex-rate binding: the fetch of VertexIndex v reads [v * stride, v * stride + kMaxElemBytes) from the base.
//     The draw's VertexIndex values are [vertex_offset, vertex_offset + count) for a direct non-indexed draw, and
//     index + vertex_offset for a direct indexed draw (the indices read on the CPU, see BindVertexBuffers). Every
//     fetch lies below last * stride + kMaxElemBytes, so that is the size.
//   - instance-rate binding with divisor d: the fetch index is instance / d, with instance running over
//     [first_instance, first_instance + instance_count) (Vulkan's gl_InstanceIndex includes firstInstance). Every
//     fetch lies below (last_instance / d) * stride + kMaxElemBytes.
//   - anything not known exactly keeps the V# size: an indirect draw, a vertex offset or first instance read from user
//     data, primitive restart, an index range that is not CPU-readable or was written by the GPU and not synced, more
//     than kMaxIndexScan indices, a joined run of the MDI path (its joiners reuse the head's binds), a zero count.
// Reads past a binding size return zero (robustBufferAccess2 is required, vk_instance.cpp), not a fault, so a size
// too small would show as wrong data, not a crash. The narrowing only ever lowers the size, never the base.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace SotcVertexExtent {

/// The narrowing is on with SOTC_VERTEX_EXTENT=1; otherwise nothing changes.
inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_VERTEX_EXTENT");
    return v && v[0] == '1';
}();

/// The widest vertex attribute element (a vec4 of 32-bit components, see SurfaceFormats()).
inline constexpr uint64_t kMaxElemBytes = 16;

/// Largest index count scanned on the CPU for one draw; a bigger draw keeps the V# sizes.
inline constexpr uint32_t kMaxIndexScan = 1u << 20;

/// What BindVertexBuffers may narrow for one direct draw. enabled is false for every other bind.
struct Draw {
    bool enabled = false;
    bool indexed = false;
    uint32_t index_offset = 0;     ///< the index_offset of Draw: the index buffer starts there (indexed only)
    uint32_t vertex_offset = 0;    ///< firstVertex of a draw, or the base vertex (vertexOffset) of a drawIndexed
    bool vertex_exact = false;     ///< vertex_offset is in the draw packet, not read from user data
    uint32_t instance_offset = 0;  ///< firstInstance
    bool instance_exact = false;   ///< instance_offset is in the draw packet (or 0), not read from user data
};

/// Counters of the 5 s report, reset by Report.
struct Counters {
    std::atomic<uint64_t> draws{0};     ///< direct draws that may narrow
    std::atomic<uint64_t> bindings{0};  ///< vertex bindings whose size shrank
    std::atomic<uint64_t> bytes{0};     ///< binding bytes not bound (V# size minus narrowed size)
    std::atomic<uint64_t> scans{0};     ///< index ranges read on the CPU
};
inline Counters g_counters;

/// The size a binding is bound with: its end when that is smaller than the V# size.
inline uint64_t Narrow(uint64_t size, uint64_t end) {
    return end < size ? end : size;
}

/// The end of the bytes read by the fetch of element `last` of a binding with `stride` (see the header comment).
inline uint64_t FetchEnd(uint64_t stride, uint64_t last) {
    return stride * last + kMaxElemBytes;
}

/// Appends the window line of the 5 s report, only while the switch is on.
inline void Report(std::string& out, double seconds) {
    if (!kOn) {
        return;
    }
    const uint64_t draws = g_counters.draws.exchange(0, std::memory_order_relaxed);
    const uint64_t bindings = g_counters.bindings.exchange(0, std::memory_order_relaxed);
    const uint64_t bytes = g_counters.bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t scans = g_counters.scans.exchange(0, std::memory_order_relaxed);
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC vertex extent (%.1f s): %llu direct draws, %llu vertex bindings narrowed, "
                  "%.2f MiB of binding size not bound, %llu index ranges scanned\n",
                  seconds, static_cast<unsigned long long>(draws),
                  static_cast<unsigned long long>(bindings),
                  static_cast<double>(bytes) / (1024.0 * 1024.0),
                  static_cast<unsigned long long>(scans));
    out += line;
}

} // namespace SotcVertexExtent
