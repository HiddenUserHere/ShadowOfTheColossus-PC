// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/passes/sotc_generic_buffers.h"
#include "shader_recompiler/ir/reinterpret.h"
#include "video_core/amdgpu/resource.h"

namespace Shader::Optimization {

struct FormatInfo {
    AmdGpu::DataFormat data_format;
    AmdGpu::NumberFormat num_format;
    AmdGpu::CompMapping swizzle;
    AmdGpu::NumberConversion num_conversion;
    u32 num_components;
};

static bool IsBufferFormatLoad(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadBufferFormatF32;
}

static bool IsBufferFormatStore(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::StoreBufferFormatF32;
}

// sotc: the data formats the load and store switches below handle. The V# and MTBUF data format
// fields are 4 bits wide, so the only value outside the table is 15, which no format is defined
// for. Such a descriptor is treated like a null sharp instead of aborting the translation.
static bool IsKnownBufferDataFormat(const AmdGpu::DataFormat data_format) {
    switch (data_format) {
    case AmdGpu::DataFormat::Format8:
    case AmdGpu::DataFormat::Format8_8:
    case AmdGpu::DataFormat::Format8_8_8_8:
    case AmdGpu::DataFormat::Format16:
    case AmdGpu::DataFormat::Format16_16:
    case AmdGpu::DataFormat::Format16_16_16_16:
    case AmdGpu::DataFormat::Format10_11_11:
    case AmdGpu::DataFormat::Format2_10_10_10:
    case AmdGpu::DataFormat::Format32:
    case AmdGpu::DataFormat::Format32_32:
    case AmdGpu::DataFormat::Format32_32_32:
    case AmdGpu::DataFormat::Format32_32_32_32:
        return true;
    default:
        return false;
    }
}

// sotc: 32-bit formats move raw dwords, so the number format does not change the loaded or stored
// bits. An unexpected number format (Snorm, reachable from V#/MTBUF) is passed through.
static void CheckRawNumberFormat(const FormatInfo& format_info) {
    if (format_info.num_format != AmdGpu::NumberFormat::Uint &&
        format_info.num_format != AmdGpu::NumberFormat::Sint &&
        format_info.num_format != AmdGpu::NumberFormat::Float) {
        AmdGpu::ReportFormatFallback("32-bit buffer number format",
                                     u32(format_info.data_format),
                                     u32(format_info.num_format));
    }
}

static IR::Value LoadBufferFormat(IR::IREmitter& ir, const IR::Value handle, const IR::U32 address,
                                  const IR::BufferInstInfo info, const FormatInfo& format_info) {
    IR::Value interpreted;
    switch (format_info.data_format) {
    case AmdGpu::DataFormat::FormatInvalid:
        interpreted = ir.Imm32(0.f);
        break;
    case AmdGpu::DataFormat::Format8: {
        const auto raw = ir.UConvert(32, ir.LoadBufferU8(handle, address, info));
        const auto unpacked = ir.Unpack4x8(format_info.num_format, raw);
        interpreted = ir.CompositeExtract(unpacked, 0);
        break;
    }
    case AmdGpu::DataFormat::Format8_8: {
        const auto raw = ir.UConvert(32, ir.LoadBufferU16(handle, address, info));
        const auto unpacked = ir.Unpack4x8(format_info.num_format, raw);
        interpreted = ir.CompositeConstruct(ir.CompositeExtract(unpacked, 0),
                                            ir.CompositeExtract(unpacked, 1));
        break;
    }
    case AmdGpu::DataFormat::Format8_8_8_8:
        interpreted = ir.Unpack4x8(format_info.num_format,
                                   IR::U32{ir.LoadBufferU32(1, handle, address, info)});
        break;
    case AmdGpu::DataFormat::Format16: {
        const auto raw = ir.UConvert(32, ir.LoadBufferU16(handle, address, info));
        const auto unpacked = ir.Unpack2x16(format_info.num_format, raw);
        interpreted = ir.CompositeExtract(unpacked, 0);
        break;
    }
    case AmdGpu::DataFormat::Format16_16:
        interpreted = ir.Unpack2x16(format_info.num_format,
                                    IR::U32{ir.LoadBufferU32(1, handle, address, info)});
        break;
    case AmdGpu::DataFormat::Format10_11_11:
        interpreted = ir.Unpack10_11_11(format_info.num_format,
                                        IR::U32{ir.LoadBufferU32(1, handle, address, info)});
        break;
    case AmdGpu::DataFormat::Format2_10_10_10:
        interpreted = ir.Unpack2_10_10_10(format_info.num_format,
                                          IR::U32{ir.LoadBufferU32(1, handle, address, info)});
        break;
    case AmdGpu::DataFormat::Format16_16_16_16: {
        const auto raw = ir.LoadBufferU32(2, handle, address, info);
        interpreted = ir.CompositeConstruct(
            ir.Unpack2x16(format_info.num_format, IR::U32{ir.CompositeExtract(raw, 0)}),
            ir.Unpack2x16(format_info.num_format, IR::U32{ir.CompositeExtract(raw, 1)}));
        break;
    }
    case AmdGpu::DataFormat::Format32:
    case AmdGpu::DataFormat::Format32_32:
    case AmdGpu::DataFormat::Format32_32_32:
    case AmdGpu::DataFormat::Format32_32_32_32: {
        CheckRawNumberFormat(format_info);
        interpreted = ir.LoadBufferF32(format_info.num_components, handle, address, info);
        break;
    }
    default:
        UNREACHABLE_MSG("Unsupported buffer data format: {}", format_info.data_format);
    }

    // Pad to 4 components and apply additional modifications.
    boost::container::static_vector<IR::Value, 4> components;
    for (u32 i = 0; i < 4; i++) {
        if (i < format_info.num_components) {
            const auto component =
                IR::F32{format_info.num_components == 1 ? interpreted
                                                        : ir.CompositeExtract(interpreted, i)};
            components.push_back(
                ApplyReadNumberConversion(ir, component, format_info.num_conversion));
        } else {
            components.push_back(ir.Imm32(0.f));
        }
    }
    const auto swizzled = ApplySwizzle(ir, ir.CompositeConstruct(components), format_info.swizzle);
    return swizzled;
}

// sotc: SOTC_GENERIC_BUFFERS=2. The formatted load of a generic buffer (GenericFormatBuffer): the address is in
// bytes (resource_patching_pass.cpp), the format and dst_sel are read from V# dword 3 at run time. Four dwords are
// loaded from the dword the element starts in; the first element dword is funnel-shifted from the first two, so the
// byte and half-word aligned 8- and 16-bit formats read right (wider formats are dword aligned, as in
// BufferAddressShift). Every format of kGenericBufferFormats is decoded the way LoadBufferFormat decodes it, and the
// one the V# names is selected; a pair outside the table never reaches this code (its key keeps the real format).
// sotc: SOTC_SPV_FUNCS. The SPIR-V decode function (emit_spirv_generic_format.cpp) applies no number conversion, so
// it takes the generic path only when every table entry has none. The table is fixed; this keeps the rule explicit.
static bool GenericDecodeHasNoConversion() {
    for (const auto& pair : kGenericBufferFormats) {
        if (AmdGpu::MapNumberConversion(AmdGpu::NumberFormat(pair.num_format),
                                        AmdGpu::DataFormat(pair.data_format)) !=
            AmdGpu::NumberConversion::None) {
            return false;
        }
    }
    return true;
}

static IR::Value LoadBufferFormatGeneric(IR::IREmitter& ir, const IR::Value handle, const IR::U32 byte_address,
                                         const IR::BufferInstInfo info, const BufferResource& desc,
                                         Info& program_info) {
    const IR::U32 dword3 = *SotcSharpDword(ir, desc, 3);
    // num_format (bits 12..14) | data_format (bits 15..18) << 3
    const IR::U32 format_code = ir.BitFieldExtract(dword3, ir.Imm32(12), ir.Imm32(7));
    const IR::U32 dword_address = IR::U32{ir.ShiftRightLogical(byte_address, ir.Imm32(2))};
    const IR::U32 bit_offset =
        IR::U32{ir.ShiftLeftLogical(IR::U32{ir.BitwiseAnd(byte_address, ir.Imm32(3))}, ir.Imm32(3))};
    const auto raw = ir.LoadBufferU32(4, handle, dword_address, info);
    std::array<IR::U32, 4> d;
    for (u32 i = 0; i < 4; ++i) {
        d[i] = IR::U32{ir.CompositeExtract(raw, i)};
    }
    const IR::U32 high_part = IR::U32{
        ir.Select(ir.IEqual(bit_offset, ir.Imm32(0)), ir.Imm32(0),
                  ir.ShiftLeftLogical(d[1], IR::U32{ir.ISub(ir.Imm32(32), bit_offset)}))};
    d[0] = IR::U32{ir.BitwiseOr(IR::U32{ir.ShiftRightLogical(d[0], bit_offset)}, high_part)};

    // sotc: SOTC_SPV_FUNCS=1 (default). The 17-way decode and the swizzle are one SPIR-V function, called here once per
    // load. The swizzle reads dst_sel from dword 3, so the opcode takes that dword as well as the format code.
    // SOTC_SPV_FUNCS=0 keeps the decode inline below.
    if (SpvFuncs() && GenericDecodeHasNoConversion()) {
        program_info.uses_generic_format_decode = true;
        return ir.UnpackGenericBufferFormat(format_code, ir.CompositeConstruct(d[0], d[1], d[2], d[3]),
                                            dword3);
    }

    const auto zero = ir.Imm32(0.f);
    std::array<IR::F32, 4> result{zero, zero, zero, zero};
    for (const auto& pair : kGenericBufferFormats) {
        const auto data_format = AmdGpu::DataFormat(pair.data_format);
        const auto num_format = AmdGpu::NumberFormat(pair.num_format);
        std::array<IR::F32, 4> c{zero, zero, zero, zero};
        const auto extract = [&](const IR::Value& vector, u32 first, u32 count) {
            for (u32 i = 0; i < count; ++i) {
                c[first + i] = IR::F32{ir.CompositeExtract(vector, i)};
            }
        };
        switch (data_format) {
        case AmdGpu::DataFormat::Format8:
            extract(ir.Unpack4x8(num_format, d[0]), 0, 1);
            break;
        case AmdGpu::DataFormat::Format8_8:
            extract(ir.Unpack4x8(num_format, d[0]), 0, 2);
            break;
        case AmdGpu::DataFormat::Format8_8_8_8:
            extract(ir.Unpack4x8(num_format, d[0]), 0, 4);
            break;
        case AmdGpu::DataFormat::Format16:
            extract(ir.Unpack2x16(num_format, d[0]), 0, 1);
            break;
        case AmdGpu::DataFormat::Format16_16:
            extract(ir.Unpack2x16(num_format, d[0]), 0, 2);
            break;
        case AmdGpu::DataFormat::Format16_16_16_16:
            extract(ir.Unpack2x16(num_format, d[0]), 0, 2);
            extract(ir.Unpack2x16(num_format, d[1]), 2, 2);
            break;
        case AmdGpu::DataFormat::Format10_11_11:
            extract(ir.Unpack10_11_11(num_format, d[0]), 0, 3);
            break;
        case AmdGpu::DataFormat::Format2_10_10_10:
            extract(ir.Unpack2_10_10_10(num_format, d[0]), 0, 4);
            break;
        case AmdGpu::DataFormat::Format32:
        case AmdGpu::DataFormat::Format32_32:
        case AmdGpu::DataFormat::Format32_32_32:
        case AmdGpu::DataFormat::Format32_32_32_32:
            for (u32 i = 0; i < AmdGpu::NumComponents(data_format); ++i) {
                c[i] = ir.BitCast<IR::F32>(d[i]);
            }
            break;
        default:
            UNREACHABLE_MSG("Generic buffer data format {} has no decoder", data_format);
        }
        const auto conversion = AmdGpu::MapNumberConversion(num_format, data_format);
        for (auto& component : c) {
            component = ApplyReadNumberConversion(ir, component, conversion);
        }
        // AmdGpu::RemapSwizzle: a single-channel format answers an Alpha selector with its one component.
        if (AmdGpu::NumComponents(data_format) == 1) {
            c[3] = c[0];
        }
        const IR::U1 selected =
            ir.IEqual(format_code, ir.Imm32(u32{pair.num_format} | (u32{pair.data_format} << 3)));
        for (u32 i = 0; i < 4; ++i) {
            result[i] = IR::F32{ir.Select(selected, c[i], result[i])};
        }
    }

    // dst_sel_x..w: 0 zero, 1 one, 4..7 the components (2 and 3 are reserved, read as zero: CompMapping::Apply).
    const auto one = ir.Imm32(1.f);
    std::array<IR::Value, 4> swizzled;
    for (u32 i = 0; i < 4; ++i) {
        const IR::U32 select = ir.BitFieldExtract(dword3, ir.Imm32(3 * i), ir.Imm32(3));
        IR::F32 value = zero;
        for (u32 component = 0; component < 4; ++component) {
            value = IR::F32{ir.Select(ir.IEqual(select, ir.Imm32(4 + component)), result[component], value)};
        }
        swizzled[i] = ir.Select(ir.IEqual(select, ir.Imm32(1)), one, value);
    }
    return ir.CompositeConstruct(swizzled[0], swizzled[1], swizzled[2], swizzled[3]);
}

static void StoreBufferFormat(IR::IREmitter& ir, const IR::Value handle, const IR::U32 address,
                              const IR::Value& value, const IR::BufferInstInfo info,
                              const FormatInfo& format_info) {
    // Extract actual number of components and apply additional modifications.
    const auto swizzled = ApplySwizzle(ir, value, format_info.swizzle.Inverse());
    boost::container::static_vector<IR::Value, 4> components;
    for (u32 i = 0; i < format_info.num_components; i++) {
        const auto component = IR::F32{ir.CompositeExtract(swizzled, i)};
        components.push_back(ApplyWriteNumberConversion(ir, component, format_info.num_conversion));
    }
    const auto real_value =
        components.size() == 1 ? components[0] : ir.CompositeConstruct(components);

    switch (format_info.data_format) {
    case AmdGpu::DataFormat::FormatInvalid:
        break;
    case AmdGpu::DataFormat::Format8: {
        const auto packed =
            ir.Pack4x8(format_info.num_format, ir.CompositeConstruct(real_value, ir.Imm32(0.f),
                                                                     ir.Imm32(0.f), ir.Imm32(0.f)));
        ir.StoreBufferU8(handle, address, ir.UConvert(8, packed), info);
        break;
    }
    case AmdGpu::DataFormat::Format8_8: {
        const auto packed = ir.Pack4x8(format_info.num_format,
                                       ir.CompositeConstruct(ir.CompositeExtract(real_value, 0),
                                                             ir.CompositeExtract(real_value, 1),
                                                             ir.Imm32(0.f), ir.Imm32(0.f)));
        ir.StoreBufferU16(handle, address, ir.UConvert(16, packed), info);
        break;
    }
    case AmdGpu::DataFormat::Format8_8_8_8: {
        auto packed = ir.Pack4x8(format_info.num_format, real_value);
        ir.StoreBufferU32(1, handle, address, packed, info);
        break;
    }
    case AmdGpu::DataFormat::Format16: {
        const auto packed =
            ir.Pack2x16(format_info.num_format, ir.CompositeConstruct(real_value, ir.Imm32(0.f)));
        ir.StoreBufferU16(handle, address, ir.UConvert(16, packed), info);
        break;
    }
    case AmdGpu::DataFormat::Format16_16: {
        const auto packed = ir.Pack2x16(format_info.num_format, real_value);
        ir.StoreBufferU32(1, handle, address, packed, info);
        break;
    }
    case AmdGpu::DataFormat::Format10_11_11: {
        const auto packed = ir.Pack10_11_11(format_info.num_format, real_value);
        ir.StoreBufferU32(1, handle, address, packed, info);
        break;
    }
    case AmdGpu::DataFormat::Format2_10_10_10: {
        const auto packed = ir.Pack2_10_10_10(format_info.num_format, real_value);
        ir.StoreBufferU32(1, handle, address, packed, info);
        break;
    }
    case AmdGpu::DataFormat::Format16_16_16_16: {
        const auto packed = ir.CompositeConstruct(
            ir.Pack2x16(format_info.num_format,
                        ir.CompositeConstruct(ir.CompositeExtract(real_value, 0),
                                              ir.CompositeExtract(real_value, 1))),
            ir.Pack2x16(format_info.num_format,
                        ir.CompositeConstruct(ir.CompositeExtract(real_value, 2),
                                              ir.CompositeExtract(real_value, 3))));
        ir.StoreBufferU32(2, handle, address, packed, info);
        break;
    }
    case AmdGpu::DataFormat::Format32:
    case AmdGpu::DataFormat::Format32_32:
    case AmdGpu::DataFormat::Format32_32_32:
    case AmdGpu::DataFormat::Format32_32_32_32: {
        CheckRawNumberFormat(format_info);
        ir.StoreBufferF32(format_info.num_components, handle, address, real_value, info);
        break;
    }
    default:
        UNREACHABLE_MSG("Unsupported buffer data format: {}", format_info.data_format);
    }
}

static void LowerBufferFormatInst(IR::Block& block, IR::Inst& inst, Info& info) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const auto flags = inst.Flags<IR::BufferInstInfo>();
    const auto desc{info.buffers[inst.Arg(0).U32()]};
    const auto buffer{desc.GetSharp(info)};

    const auto is_inst_typed = flags.inst_data_fmt != AmdGpu::DataFormat::FormatInvalid;
    // sotc: SOTC_GENERIC_BUFFERS=2, the same decision as PatchBufferArgs (which made the address a byte address).
    if (IsBufferFormatLoad(inst) && !is_inst_typed && SotcGenericFormatLoads(desc, buffer)) {
        inst.ReplaceUsesWithAndRemove(
            LoadBufferFormatGeneric(ir, inst.Arg(0), IR::U32{inst.Arg(1)}, flags, desc, info));
        return;
    }
    const auto data_format =
        is_inst_typed ? AmdGpu::RemapDataFormat(flags.inst_data_fmt.Value()) : buffer.GetDataFmt();
    const auto format_info = FormatInfo{
        .data_format = data_format,
        .num_format = is_inst_typed
                          ? AmdGpu::RemapNumberFormat(flags.inst_num_fmt.Value(), data_format)
                          : buffer.GetNumberFmt(),
        .swizzle = is_inst_typed
                       ? AmdGpu::RemapSwizzle(flags.inst_data_fmt.Value(), AmdGpu::IdentityMapping)
                       : buffer.DstSelect(),
        .num_conversion = is_inst_typed ? AmdGpu::MapNumberConversion(flags.inst_num_fmt.Value(),
                                                                      flags.inst_data_fmt.Value())
                                        : buffer.GetNumberConversion(),
        .num_components = AmdGpu::NumComponents(data_format),
    };

    if (data_format == AmdGpu::DataFormat::FormatInvalid ||
        !IsKnownBufferDataFormat(data_format)) {
        if (data_format == AmdGpu::DataFormat::FormatInvalid) {
            LOG_WARNING(Render_Recompiler, "Skipping lowering for null buffer sharp");
        } else {
            // sotc: reported once per combination; the access reads zero / is dropped, as for null.
            AmdGpu::ReportFormatFallback("buffer data format", u32(data_format),
                                         u32(format_info.num_format));
        }
        if (IsBufferFormatLoad(inst)) {
            inst.ReplaceUsesWithAndRemove(
                ir.CompositeConstruct(ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f)));
        } else {
            inst.Invalidate();
        }
        return;
    }

    if (IsBufferFormatLoad(inst)) {
        const auto interpreted =
            LoadBufferFormat(ir, inst.Arg(0), IR::U32{inst.Arg(1)}, flags, format_info);
        inst.ReplaceUsesWithAndRemove(interpreted);
    } else if (IsBufferFormatStore(inst)) {
        StoreBufferFormat(ir, inst.Arg(0), IR::U32{inst.Arg(1)}, inst.Arg(2), flags, format_info);
        inst.Invalidate();
    }
}

void LowerBufferFormatToRaw(IR::Program& program) {
    auto& info = program.info;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (IsBufferFormatLoad(inst) || IsBufferFormatStore(inst)) {
                LowerBufferFormatInst(*block, inst, info);
            }
        }
    }
}

} // namespace Shader::Optimization
