// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include "common/types.h"

namespace Shader {

/// sotc: SOTC_HEAP stage variants (ShaderParams::variant, Info::variant). A pipeline has at most one heap stage (the
/// fragment of a graphics pipeline, the compute stage of a compute one); its other stages are plain: they read their
/// images and samplers through descriptors and take no bindless slot words. Default is today's compile.
inline constexpr u8 StageVariantDefault = 0;
inline constexpr u8 StageVariantPlain = 1;
inline constexpr u8 StageVariantHeap = 2;

/**
 * Compilation parameters used to identify and locate a guest shader program.
 */
struct ShaderParams {
    static constexpr u32 NumShaderUserData = 16;

    std::span<const u32, NumShaderUserData> user_data;
    std::span<const u32> code;
    u64 hash;
    /// sotc: SOTC_HEAP stage variant (StageVariantDefault unless the pipeline cache chose another). A variant other than
    /// the default is mixed into hash by the caller, so every cache keyed by hash keeps the variants apart.
    u8 variant{StageVariantDefault};

    VAddr Base() const noexcept {
        return reinterpret_cast<VAddr>(code.data());
    }
};

} // namespace Shader
