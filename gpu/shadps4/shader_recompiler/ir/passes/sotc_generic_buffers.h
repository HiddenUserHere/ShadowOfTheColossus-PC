// SPDX-FileCopyrightText: Copyright 2026 sotc-pc
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_GENERIC_BUFFERS. Run-time reads of a buffer's V# words, for the generic stride
// (resource_patching_pass.cpp) and the generic formatted loads (lower_buffer_format_to_raw.cpp).

#pragma once

#include <optional>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"

namespace Shader::Optimization {

/// Dword `index` (0..3) of the buffer's V# as the shader sees it at run time: a flattened user data dword where
/// SharpFetch loads it, or the immediate the code built the V# with. The post op is applied (an OR into dword 1).
/// nullopt when the fetch is not a load (an invalid fetch, whose sharp is always the null buffer).
inline std::optional<IR::U32> SotcSharpDword(IR::IREmitter& ir, const BufferResource& desc, u32 index) {
    const auto& fetch = desc.sharp_fetch;
    std::optional<IR::U32> dword;
    if (fetch.summary == SharpFetch<AmdGpu::Buffer>::Summary::SingleLoad) {
        dword = ir.LoadFlatbufDword(u32{fetch.offsets[0]} + index);
    } else if (fetch.summary == SharpFetch<AmdGpu::Buffer>::Summary::MultiLoad) {
        dword = (fetch.load_mask >> index) & 1 ? ir.LoadFlatbufDword(u32{fetch.offsets[index]})
                                                : ir.Imm32(fetch.immediates[index]);
    }
    if (dword && index == 1 && desc.post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
        dword = IR::U32{ir.BitwiseOr(*dword, ir.Imm32(desc.post_op_dw1_mask))};
    }
    return dword;
}

/// True when the buffer's formatted loads decode the V# format at run time (SOTC_GENERIC_BUFFERS=2). The fetch must
/// be a load, so the shader can read dword 3.
inline bool SotcGenericFormatLoads(const BufferResource& desc, const AmdGpu::Buffer& sharp) {
    return GenericFormatBuffer(desc, sharp) &&
           (desc.sharp_fetch.summary == SharpFetch<AmdGpu::Buffer>::Summary::SingleLoad ||
            desc.sharp_fetch.summary == SharpFetch<AmdGpu::Buffer>::Summary::MultiLoad);
}

} // namespace Shader::Optimization
