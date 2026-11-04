// SPDX-FileCopyrightText: Copyright 2026 sotc-pc
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_SPV_FUNCS. The generic formatted buffer decode (IR::Opcode::UnpackGenericBufferFormat, emitted by
// LowerBufferFormatToRaw) as one DontInline SPIR-V function per shader. Before, the decode of all 17 formats of
// kGenericBufferFormats was inlined at every formatted load. NVIDIA compiles a shader much faster when a big helper is
// called from its sites instead (forums.developer.nvidia.com/t/381276).
//
// The function body does the same operations as LoadBufferFormatGeneric (ir/passes/lower_buffer_format_to_raw.cpp)
// for each format: the same Unpack and BitCast ops, through the same EmitUnpack* functions as the IR emitters.
// Only one format's decode runs (a switch on the format code), and the result is then swizzled by dst_sel.

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/info.h"

namespace Shader::Backend::SPIRV {

namespace {

using Comps = std::array<Id, 4>;

// IREmitter::Unpack4x8 for the number formats the generic table uses (ir_emitter.cpp).
Id UnpackGeneric4x8(EmitContext& ctx, AmdGpu::NumberFormat num_format, Id value) {
    switch (num_format) {
    case AmdGpu::NumberFormat::Unorm:
        return EmitUnpackUnorm4x8(ctx, value);
    case AmdGpu::NumberFormat::Snorm:
        return EmitUnpackSnorm4x8(ctx, value);
    case AmdGpu::NumberFormat::Uint:
        return EmitUnpackUint4x8(ctx, value);
    case AmdGpu::NumberFormat::Sint:
        return EmitUnpackSint4x8(ctx, value);
    default:
        return EmitUnpackUint4x8(ctx, value); // as IREmitter::Unpack4x8 reads Float bits as Uint
    }
}

// IREmitter::Unpack2x16 for the number formats the generic table uses.
Id UnpackGeneric2x16(EmitContext& ctx, AmdGpu::NumberFormat num_format, Id value) {
    switch (num_format) {
    case AmdGpu::NumberFormat::Unorm:
        return EmitUnpackUnorm2x16(ctx, value);
    case AmdGpu::NumberFormat::Snorm:
        return EmitUnpackSnorm2x16(ctx, value);
    case AmdGpu::NumberFormat::Uint:
        return EmitUnpackUint2x16(ctx, value);
    case AmdGpu::NumberFormat::Sint:
        return EmitUnpackSint2x16(ctx, value);
    case AmdGpu::NumberFormat::Float:
        return EmitUnpackHalf2x16(ctx, value);
    default:
        return EmitUnpackUint2x16(ctx, value);
    }
}

// IREmitter::Unpack2_10_10_10 for the number formats the generic table uses.
Id UnpackGeneric2_10_10_10(EmitContext& ctx, AmdGpu::NumberFormat num_format, Id value) {
    switch (num_format) {
    case AmdGpu::NumberFormat::Unorm:
        return EmitUnpackUnorm2_10_10_10(ctx, value);
    case AmdGpu::NumberFormat::Snorm:
        return EmitUnpackSnorm2_10_10_10(ctx, value);
    case AmdGpu::NumberFormat::Uint:
        return EmitUnpackUint2_10_10_10(ctx, value);
    case AmdGpu::NumberFormat::Sint:
        return EmitUnpackSint2_10_10_10(ctx, value);
    default:
        return EmitUnpackUint2_10_10_10(ctx, value);
    }
}

// The decode of one kGenericBufferFormats entry, from the dwords after the byte-offset shift (d). Lanes past the
// format's components are zero. Only the operations of LoadBufferFormatGeneric's per-format switch are emitted; the
// number conversion is None for every table entry (checked by the caller in lower_buffer_format_to_raw.cpp).
Comps DecodeGenericFormat(EmitContext& ctx, const GenericBufferFormatPair& pair,
                          const std::array<Id, 4>& d, Id zero) {
    const auto data_format = AmdGpu::DataFormat(pair.data_format);
    const auto num_format = AmdGpu::NumberFormat(pair.num_format);
    Comps c{zero, zero, zero, zero};
    const auto extract = [&](Id vector, u32 first, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            c[first + i] = ctx.OpCompositeExtract(ctx.F32[1], vector, i);
        }
    };
    switch (data_format) {
    case AmdGpu::DataFormat::Format8:
        extract(UnpackGeneric4x8(ctx, num_format, d[0]), 0, 1);
        break;
    case AmdGpu::DataFormat::Format8_8:
        extract(UnpackGeneric4x8(ctx, num_format, d[0]), 0, 2);
        break;
    case AmdGpu::DataFormat::Format8_8_8_8:
        extract(UnpackGeneric4x8(ctx, num_format, d[0]), 0, 4);
        break;
    case AmdGpu::DataFormat::Format16:
        extract(UnpackGeneric2x16(ctx, num_format, d[0]), 0, 1);
        break;
    case AmdGpu::DataFormat::Format16_16:
        extract(UnpackGeneric2x16(ctx, num_format, d[0]), 0, 2);
        break;
    case AmdGpu::DataFormat::Format16_16_16_16:
        extract(UnpackGeneric2x16(ctx, num_format, d[0]), 0, 2);
        extract(UnpackGeneric2x16(ctx, num_format, d[1]), 2, 2);
        break;
    case AmdGpu::DataFormat::Format10_11_11:
        // Not in kGenericBufferFormats (the IR decoder has no case for it either).
        break;
    case AmdGpu::DataFormat::Format2_10_10_10:
        extract(UnpackGeneric2_10_10_10(ctx, num_format, d[0]), 0, 4);
        break;
    case AmdGpu::DataFormat::Format32:
    case AmdGpu::DataFormat::Format32_32:
    case AmdGpu::DataFormat::Format32_32_32:
    case AmdGpu::DataFormat::Format32_32_32_32:
        for (u32 i = 0; i < AmdGpu::NumComponents(data_format); ++i) {
            c[i] = ctx.OpBitcast(ctx.F32[1], d[i]);
        }
        break;
    default:
        break;
    }
    // AmdGpu::RemapSwizzle: a single-channel format answers an Alpha selector with its one component.
    if (AmdGpu::NumComponents(data_format) == 1) {
        c[3] = c[0];
    }
    return c;
}

} // namespace

Id EmitUnpackGenericBufferFormat(EmitContext& ctx, Id format_code, Id dwords, Id dst_sel) {
    return ctx.OpFunctionCall(ctx.F32[4], ctx.generic_format_func, format_code, dwords, dst_sel);
}

// Defined once per shader when the shader has a generic formatted load (DefineFunctions). The format code selects one
// decode with an OpSwitch; a code outside the table decodes to zero, as the IR decoder's default did. Then dst_sel
// (3 bits per channel, from bit 0: 0 zero, 1 one, 4..7 the components) picks each output channel.
Id EmitContext::DefineGenericBufferFormat() {
    const Id func_type{TypeFunction(F32[4], U32[1], U32[4], U32[1])};
    const Id func{OpFunction(F32[4], spv::FunctionControlMask::DontInline, func_type)};
    const Id format_code{OpFunctionParameter(U32[1])};
    const Id dwords{OpFunctionParameter(U32[4])};
    const Id dst_sel{OpFunctionParameter(U32[1])};
    Name(func, "generic_buffer_format");
    AddLabel();

    // The result variable must be declared in the first block, before any other instruction.
    const Id zero{ConstF32(0.f)};
    const Id zero_vec{ConstantComposite(F32[4], zero, zero, zero, zero)};
    const Id result_var{AddLocalVariable(TypePointer(spv::StorageClass::Function, F32[4]),
                                         spv::StorageClass::Function, zero_vec)};
    std::array<Id, 4> d{};
    for (u32 i = 0; i < 4; ++i) {
        d[i] = OpCompositeExtract(U32[1], dwords, i);
    }

    const Id merge{OpLabel()};
    std::vector<Sirit::Literal> literals;
    std::vector<Id> labels;
    for (const auto& pair : kGenericBufferFormats) {
        literals.emplace_back(u32{pair.num_format} | (u32{pair.data_format} << 3));
        labels.push_back(OpLabel());
    }
    OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    OpSwitch(format_code, merge, literals, labels);
    for (size_t i = 0; i < labels.size(); ++i) {
        AddLabel(labels[i]);
        const Comps c{DecodeGenericFormat(*this, kGenericBufferFormats[i], d, zero)};
        OpStore(result_var, OpCompositeConstruct(F32[4], c[0], c[1], c[2], c[3]));
        OpBranch(merge);
    }
    AddLabel(merge);

    // dst_sel: the swizzle of LoadBufferFormatGeneric. Reserved selectors 2 and 3 read as zero.
    const Id result{OpLoad(F32[4], result_var)};
    const Id one{ConstF32(1.f)};
    std::array<Id, 4> out{};
    for (u32 i = 0; i < 4; ++i) {
        const Id select{OpBitFieldUExtract(U32[1], dst_sel, ConstU32(3 * i), ConstU32(3U))};
        Id value{zero};
        for (u32 c = 0; c < 4; ++c) {
            value = OpSelect(F32[1], OpIEqual(U1[1], select, ConstU32(4 + c)),
                             OpCompositeExtract(F32[1], result, c), value);
        }
        out[i] = OpSelect(F32[1], OpIEqual(U1[1], select, ConstU32(1U)), one, value);
    }
    OpReturnValue(OpCompositeConstruct(F32[4], out[0], out[1], out[2], out[3]));
    OpFunctionEnd();
    return func;
}

} // namespace Shader::Backend::SPIRV
