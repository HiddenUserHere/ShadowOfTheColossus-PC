// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include "common/types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 buffer{};

    auto operator<=>(const Bindings&) const = default;
};

/// sotc: SOTC_VS_BINDING_BASE=1 (off by default). The vertex stage's bindings start at a fixed base instead of
/// where the pixel stage's end (the pixel stage is bound first), so a vertex program has one key whatever pixel
/// program it is drawn with: its shaders can be translated before any draw (SOTC_PRETRANSLATE). Vulkan allows the
/// gap in the binding numbers. A pixel stage that does not fit below the base keeps today's numbering; the pipeline
/// cache, the rasterizer's bind loop and the descriptor set layout apply the same rule (VsStartBindings).
inline bool VsBindingBaseOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_VS_BINDING_BASE");
        return v && v[0] == '1';
    }();
    return on;
}
/// Descriptor binding of the vertex stage's first resource (set 0): SOTC_VS_UNIFIED_BASE (default 48, diagnostics).
inline u32 VsUnifiedBase() {
    static const u32 base = [] {
        const char* v = std::getenv("SOTC_VS_UNIFIED_BASE");
        const long n = v ? std::strtol(v, nullptr, 10) : 48;
        return n > 0 && n < 256 ? static_cast<u32>(n) : 48u;
    }();
    return base;
}
inline constexpr u32 kVsBufferBase = 16;  ///< push constant buffer slot of its first buffer (of NUM_BUFFERS = 40)
/// SOTC_VS_BUFFER_BASE (diagnostics): the buffer slot base, -1 = where the stages before ended (the old numbering).
inline int VsBufferBase() {
    static const int base = [] {
        const char* v = std::getenv("SOTC_VS_BUFFER_BASE");
        return v ? static_cast<int>(std::strtol(v, nullptr, 10)) : static_cast<int>(kVsBufferBase);
    }();
    return base;
}

/// The vertex stage's start from the bindings the stages before it ended at. staged: SOTC_STAGE_BINDINGS (the
/// stage numbers its descriptors from 0 in its own set; only the shared buffer slots move).
inline Bindings VsStartBindings(Bindings prev, bool staged) {
    const int buffer_base = VsBufferBase();
    if (!VsBindingBaseOn() || prev.unified > VsUnifiedBase() ||
        (buffer_base >= 0 && prev.buffer > static_cast<u32>(buffer_base))) {
        return prev;
    }
    return {staged ? prev.unified : VsUnifiedBase(), buffer_base >= 0 ? static_cast<u32>(buffer_base) : prev.buffer};
}

} // namespace Shader::Backend
