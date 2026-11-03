// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_SHADER_EXTENT=1 (off by default). Binds an unbounded read-only V# that the shader builds itself over
// the bytes its loads can reach (BufferResource::sotc_read_extent, proven at translation by
// shader_recompiler/ir/passes/sotc_shader_extent_pass.cpp) instead of the 64 MiB window (SHADPS4_UNBOUNDED_BUFFER_MB),
// so the bind no longer syncs the CPU-dirty pages of the whole window. The window is rounded up to 256 bytes. A read
// past a shortened binding would return 0 (robustBufferAccess2), but the analysis proves there is none.
//
// The 5 s report (driver.cpp) counts the limited binds and the bytes they no longer bind.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace SotcShaderExtent {

inline const bool kEnabled = [] {
    const char* v = std::getenv("SOTC_SHADER_EXTENT");
    return v && v[0] == '1';
}();

inline std::atomic<uint64_t> limited_binds{0};
inline std::atomic<uint64_t> saved_bytes{0};

/// The bound size of a guest binding of `size` bytes whose V# says `vsize`: the proven extent for a read-only binding
/// with an unbounded V# and a proven extent, else `size`. Counts what it limits.
inline uint64_t Limit(bool is_written, uint32_t read_extent, uint64_t vsize, uint64_t size) {
    if (!kEnabled || is_written || read_extent == 0 || vsize < 0xFFFF0000ULL) {
        return size;
    }
    const uint64_t window = (uint64_t{read_extent} + 255) & ~uint64_t{255};
    if (window >= size) {
        return size;
    }
    limited_binds.fetch_add(1, std::memory_order_relaxed);
    saved_bytes.fetch_add(size - window, std::memory_order_relaxed);
    return window;
}

inline void Report(std::string& out, double seconds) {
    if (!kEnabled || seconds <= 0.0) {
        return;
    }
    const uint64_t binds = limited_binds.exchange(0, std::memory_order_relaxed);
    const uint64_t bytes = saved_bytes.exchange(0, std::memory_order_relaxed);
    char line[192];
    std::snprintf(line, sizeof(line),
                  "SOTC SHADER EXTENT: %llu unbounded read-only binds limited to their proven extent (%.1f/s), "
                  "%.1f MiB not bound\n",
                  static_cast<unsigned long long>(binds), double(binds) / seconds, double(bytes) / 1048576.0);
    out += line;
}

} // namespace SotcShaderExtent
