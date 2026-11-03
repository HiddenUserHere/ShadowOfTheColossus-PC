// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_NO_PREPASS (docs/re-backend/04-depth-prepass.md, section 8). Measurement only.
//  - unset or 0: nothing is counted or printed.
//  - 1: reserved for the pre-pass removal. Not implemented: the pre-pass stays and one line says so.
//  - 2: the pre-pass stays. Per draw, the draws into the main depth target are counted: the
//    colour-bearing ones (G-buffer) by depth func and depth write, and the colour-less ones
//    (depth-only). Printed with the SOTC_FPS report (driver.cpp) as "SOTC NO PREPASS: ...".
// The main depth target is learned at run time: the non-square depth target (pitch != height,
// pitch >= 512) with the largest area seen so far. Square targets (shadows) are not counted.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "video_core/amdgpu/regs.h"

namespace SotcNoPrepass {

inline const int kMode = [] {
    const char* v = std::getenv("SOTC_NO_PREPASS");
    return (v && v[0] >= '0' && v[0] <= '9') ? v[0] - '0' : 0;
}();

inline std::atomic<uint64_t> gbuffer_draws{0}, gbuffer_equal{0}, gbuffer_lequal{0},
    gbuffer_other{0}, gbuffer_writes_on{0}, depth_only_draws{0}, frames{0};
inline std::atomic<uint64_t> main_area{0}, main_dims{0}; // dims: pitch << 32 | height

/// Called once per draw with the draw registers (Rasterizer::ComputeDynamic callers).
inline void Count(const AmdGpu::Regs& regs) {
    if (kMode != 2 || !regs.depth_buffer.DepthValid()) {
        return;
    }
    const uint32_t pitch = regs.depth_buffer.Pitch();
    const uint32_t height = regs.depth_buffer.Height();
    if (pitch == height || pitch < 512) {
        return;
    }
    const uint64_t area = uint64_t{pitch} * height;
    uint64_t cur = main_area.load(std::memory_order_relaxed);
    while (area > cur) {
        if (main_area.compare_exchange_weak(cur, area, std::memory_order_relaxed)) {
            main_dims.store((uint64_t{pitch} << 32) | height, std::memory_order_relaxed);
            break;
        }
    }
    if (((uint64_t{pitch} << 32) | height) != main_dims.load(std::memory_order_relaxed)) {
        return;
    }
    bool colour = false;
    for (uint32_t cb = 0; cb < 8; ++cb) {
        colour |= regs.color_target_mask.GetMask(cb) != 0;
    }
    if (!colour) {
        depth_only_draws.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    gbuffer_draws.fetch_add(1, std::memory_order_relaxed);
    switch (regs.depth_control.depth_func) {
    case AmdGpu::CompareFunc::Equal:
        gbuffer_equal.fetch_add(1, std::memory_order_relaxed);
        break;
    case AmdGpu::CompareFunc::LessEqual:
        gbuffer_lequal.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        gbuffer_other.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    if (regs.depth_control.depth_write_enable) {
        gbuffer_writes_on.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Called once per flip while SOTC_FPS is on.
inline void NoteFrame() {
    if (kMode == 2) {
        frames.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Appends the report line of the window (counters are read and reset) to `out`.
inline void Report(std::string& out, double seconds) {
    (void)seconds;
    if (kMode == 0) {
        return;
    }
    if (kMode == 1) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            out += "SOTC NO PREPASS: mode 1 is not implemented, the pre-pass is kept\n";
        }
        return;
    }
    const uint64_t n_frames = frames.exchange(0, std::memory_order_relaxed);
    const uint64_t div = n_frames ? n_frames : 1;
    const uint64_t g = gbuffer_draws.exchange(0, std::memory_order_relaxed);
    const uint64_t e = gbuffer_equal.exchange(0, std::memory_order_relaxed);
    const uint64_t l = gbuffer_lequal.exchange(0, std::memory_order_relaxed);
    const uint64_t o = gbuffer_other.exchange(0, std::memory_order_relaxed);
    const uint64_t w = gbuffer_writes_on.exchange(0, std::memory_order_relaxed);
    const uint64_t d = depth_only_draws.exchange(0, std::memory_order_relaxed);
    char line[320];
    std::snprintf(line, sizeof line,
                  "SOTC NO PREPASS: mode %d, gbuffer draws %llu (equal %llu, lequal %llu, other "
                  "%llu, writes on %llu), depth-only draws %llu (per frame over %llu frames)\n",
                  kMode, static_cast<unsigned long long>(g / div),
                  static_cast<unsigned long long>(e / div), static_cast<unsigned long long>(l / div),
                  static_cast<unsigned long long>(o / div), static_cast<unsigned long long>(w / div),
                  static_cast<unsigned long long>(d / div),
                  static_cast<unsigned long long>(n_frames));
    out += line;
}

} // namespace SotcNoPrepass
