// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/div_ceil.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/runtime_info.h"

#include <boost/container/static_vector.hpp>
#include <fmt/format.h>
#include <spirv/unified1/spirv.hpp11>

#include <bit>
#include <numbers>
#include <string_view>

namespace Shader {

// sotc: SOTC_SHADER_ADDR. Shared with the host (runtime_info.h): the same guest buffers keep their descriptors.
bool ShaderAddrCandidate(const Info& info, const BufferResource& desc) {
    if (!info.has_flatbuf) {
        return false;
    }
    // The window words sit where the bindless slot words are: such a stage keeps its descriptors.
    if (UsesBindlessSamplers(info) || UsesBindlessImages(info)) {
        return false;
    }
    if (desc.buffer_type != BufferType::Guest ||
        desc.post_op != SharpFetchPostOp::None ||
        desc.sharp_fetch.summary == SharpFetch<AmdGpu::Buffer>::Summary::Invalid) {
        return false;
    }
    // sotc: SOTC_GCN_BUF. A formatted buffer is a candidate only when its loads decode the V# format at run time
    // (GenericFormatBuffer without the V#: SOTC_GENERIC_BUFFERS=2, read-only). Such loads are lowered to raw loads
    // on the buffer's handle, as every formatted access is (lower_buffer_format_to_raw.cpp), so the address path is
    // the same one. Whether the profile takes them is ShaderAddrBuffer's decision (not the cached Info part).
    if (desc.is_formatted && (!GenericBufferFormat() || desc.is_written)) {
        return false;
    }
    // The decision must depend only on what the compiled shader fixes: the host evaluates it again at every bind
    // with that draw's V#, and a different answer leaves the shader reading a descriptor that was never bound
    // (device lost). So no V# field (such as MTYPE 3, coherent) may enter it.
    return true;
}

// sotc: SOTC_GCN_BUF. The profile's share of the candidate rule: formatted buffers take their address from the shader
// only in table mode with gcn_buf on. Every other buffer is as before.
static bool ShaderAddrFormattedAllowed(const Profile& profile, const BufferResource& desc) {
    return !desc.is_formatted || (profile.gcn_buf != 0 && profile.shader_addr >= 3 && GcnBufFormatted());
}

bool ShaderAddrBuffer(const Profile& profile, const Info& info, const BufferResource& desc) {
    if (!profile.shader_addr || !profile.support_int64) {
        return false;
    }
    if (!ShaderAddrFormattedAllowed(profile, desc)) {
        return false;
    }
    return ShaderAddrCandidate(info, desc);
}

// sotc: the cached answer per buffer once the Info is complete (Info::bindless), the rule otherwise.
bool ShaderAddrBufferAt(const Profile& profile, const Info& info, u32 index) {
    if (!profile.shader_addr || !profile.support_int64) {
        return false;
    }
    if (index >= info.buffers.size() || !ShaderAddrFormattedAllowed(profile, info.buffers[index])) {
        return false;
    }
    if (info.bindless.ready) {
        return ((info.bindless.shader_addr_buffers >> index) & 1) != 0;
    }
    return ShaderAddrCandidate(info, info.buffers[index]);
}

} // namespace Shader

namespace Shader::Backend::SPIRV {
namespace {

std::string_view StageName(HwStage stage) {
    switch (stage) {
    case HwStage::Vertex:
        return "vs";
    case HwStage::Local:
        return "ls";
    case HwStage::Export:
        return "es";
    case HwStage::Hull:
        return "hs";
    case HwStage::Geometry:
        return "gs";
    case HwStage::Fragment:
        return "fs";
    case HwStage::Compute:
        return "cs";
    }
    UNREACHABLE_MSG("Invalid hw stage {}", u32(stage));
}

static constexpr u32 NumVertices(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return 1u;
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::LineStrip:
        return 2u;
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::RectList:
        return 3u;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        return 6u;
    case AmdGpu::PrimitiveType::AdjLineList:
        return 4u;
    default:
        UNREACHABLE();
    }
}

template <typename... Args>
void Name(EmitContext& ctx, Id object, std::string_view format_str, Args&&... args) {
    ctx.Name(object, fmt::format(fmt::runtime(format_str), StageName(ctx.hw_stage),
                                 std::forward<Args>(args)...)
                         .c_str());
}

// sotc: SOTC_SHADER_ADDR modes (profile.shader_addr): 1 addresses, 2 verify, 3 addresses through the guest page table
// (SOTC_ADDR_TABLE, addr_table.h), 4 table and verify. Table modes: the packet's mirror words are the table window
// (mirror_guest_base, mirror_size) and the table's device address (mirror_device_base), and each access is translated
// per page (TableAddresses).
bool ShaderAddrVerifyMode(const Profile& profile) {
    return profile.shader_addr == 2 || profile.shader_addr == 4;
}
bool ShaderAddrTableMode(const Profile& profile) {
    return profile.shader_addr >= 3;
}

} // Anonymous namespace

// sotc: SOTC_SHADER_ADDR: any guest buffer of the shader takes its address from the shader (spirv_emit_context.h).
bool UsesShaderAddr(const Profile& profile, const Info& info) {
    for (const auto& desc : info.buffers) {
        if (ShaderAddrBuffer(profile, info, desc)) {
            return true;
        }
    }
    return false;
}

EmitContext::EmitContext(const Profile& profile_, const RuntimeInfo& runtime_info_, Info& info_,
                         Bindings& binding_)
    : Sirit::Module(profile_.supported_spirv), info{info_}, runtime_info{runtime_info_},
      profile{profile_}, hw_stage{info.hw_stage}, sw_stage{info.sw_stage}, binding{binding_} {
    // sotc: SOTC_SHADER_ADDR buffers need physical pointers as well (UsesShaderAddr). So does a heap stage: its
    // index table and debug buffer are read and written through physical pointers (LoadTableIndex, HeapRecord).
    const bool heap = UsesHeapImages(info, profile);
    if (info.uses_dma || info.HasBdaBlockTable() || UsesShaderAddr(profile, info) || heap) {
        SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
    } else {
        SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    }
    // sotc: no per-program bytes by default, so equal code is equal SPIR-V (SpvProgramIds, info.h).
    if (SpvProgramIds()) {
        String(fmt::format("{:#x}", info.pgm_hash));
    }
    if (const u32 salt = SpvSalt(); salt != 0) { // sotc: SOTC_SPV_SALT (info.h)
        String(fmt::format("sotc salt {}", salt));
    }

    AddCapability(spv::Capability::Shader);
    if (UsesShaderAddr(profile, info) || heap) { // sotc: SOTC_SHADER_ADDR: 64-bit pointers (as emit_spirv.cpp does for the block table)
        AddCapability(spv::Capability::Int64);
        AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        AddExtension("SPV_KHR_physical_storage_buffer");
    }
    DefineArithmeticTypes();
    DefineInterfaces();
    DefineSharedMemory();
    DefineBuffers();
    DefineImagesAndSamplers();
    DefineFunctions();
}

EmitContext::~EmitContext() = default;

Id EmitContext::Def(const IR::Value& value) {
    if (!value.IsImmediate()) {
        return value.Inst()->Definition<Id>();
    }
    switch (value.Type()) {
    case IR::Type::Void:
        return Id{};
    case IR::Type::U1:
        return value.U1() ? true_value : false_value;
    case IR::Type::U32:
        return ConstU32(value.U32());
    case IR::Type::U64:
        return Constant(U64, value.U64());
    case IR::Type::F32:
        return ConstF32(value.F32());
    case IR::Type::F64:
        return Constant(F64[1], value.F64());
    case IR::Type::StringLiteral:
        return String(value.StringLiteral());
    default:
        UNREACHABLE_MSG("Immediate type {}", value.Type());
    }
}

void EmitContext::DefineArithmeticTypes() {
    void_id = Name(TypeVoid(), "void_id");
    U1[1] = Name(TypeBool(), "bool_id");
    U8 = Name(TypeUInt(8), "u8_id");
    S8 = Name(TypeSInt(8), "i8_id");
    U16 = Name(TypeUInt(16), "u16_id");
    S16 = Name(TypeSInt(16), "i16_id");
    if (info.uses_fp16) {
        F16[1] = Name(TypeFloat(16), "f16_id");
    }
    if (info.uses_fp64) {
        F64[1] = Name(TypeFloat(64), "f64_id");
    }
    F32[1] = Name(TypeFloat(32), "f32_id");
    S32[1] = Name(TypeSInt(32), "i32_id");
    U32[1] = Name(TypeUInt(32), "u32_id");
    // sotc: no 64-bit integer type where the device lacks shaderInt64 and the shader does not
    // need one (Info::uses_int64): declaring it requires the Int64 capability.
    if (profile.support_int64 || info.uses_int64) {
        U64 = Name(TypeUInt(64), "u64_id");
    }

    for (u32 i = 2; i <= 4; i++) {
        if (info.uses_fp16) {
            F16[i] = Name(TypeVector(F16[1], i), fmt::format("f16vec{}_id", i));
        }
        if (info.uses_fp64) {
            F64[i] = Name(TypeVector(F64[1], i), fmt::format("f64vec{}_id", i));
        }
        F32[i] = Name(TypeVector(F32[1], i), fmt::format("f32vec{}_id", i));
        S32[i] = Name(TypeVector(S32[1], i), fmt::format("i32vec{}_id", i));
        U32[i] = Name(TypeVector(U32[1], i), fmt::format("u32vec{}_id", i));
        U1[i] = Name(TypeVector(U1[1], i), fmt::format("bvec{}_id", i));
    }

    true_value = ConstantTrue(U1[1]);
    false_value = ConstantFalse(U1[1]);
    u8_one_value = Constant(U8, 1U);
    u8_zero_value = Constant(U8, 0U);
    u16_zero_value = Constant(U16, 0U);
    u32_one_value = ConstU32(1U);
    u32_zero_value = ConstU32(0U);
    f32_zero_value = ConstF32(0.0f);
    if (Sirit::ValidId(U64)) {
        u64_one_value = Constant(U64, 1ULL);
        u64_zero_value = Constant(U64, 0ULL);
    }

    pi_x2 = ConstF32(2.0f * float{std::numbers::pi});

    input_f32 = Name(TypePointer(spv::StorageClass::Input, F32[1]), "input_f32");
    input_u32 = Name(TypePointer(spv::StorageClass::Input, U32[1]), "input_u32");
    input_s32 = Name(TypePointer(spv::StorageClass::Input, S32[1]), "input_s32");

    output_f32 = Name(TypePointer(spv::StorageClass::Output, F32[1]), "output_f32");
    output_u32 = Name(TypePointer(spv::StorageClass::Output, U32[1]), "output_u32");
    output_s32 = Name(TypePointer(spv::StorageClass::Output, S32[1]), "output_s32");

    full_result_i32x2 = Name(TypeStruct(S32[1], S32[1]), "full_result_i32x2");
    full_result_u32x2 = Name(TypeStruct(U32[1], U32[1]), "full_result_u32x2");
    frexp_result_f32 = Name(TypeStruct(F32[1], S32[1]), "frexp_result_f32");
    if (info.uses_fp64) {
        frexp_result_f64 = Name(TypeStruct(F64[1], S32[1]), "frexp_result_f64");
    }
    if (info.uses_dma) {
        physical_pointer_type_u32 = TypePointer(spv::StorageClass::PhysicalStorageBuffer, U32[1]);
    }
}

void EmitContext::DefineInterfaces() {
    DefinePushDataBlock();
    DefineInputs();
    DefineOutputs();
}

const VectorIds& GetAttributeType(EmitContext& ctx, AmdGpu::NumberFormat fmt) {
    switch (GetNumberClass(fmt)) {
    case AmdGpu::NumberClass::Float:
        return ctx.F32;
    case AmdGpu::NumberClass::Sint:
        return ctx.S32;
    case AmdGpu::NumberClass::Uint:
        return ctx.U32;
    default:
        break;
    }
    UNREACHABLE_MSG("Invalid attribute type {}", fmt);
}

EmitContext::SpirvAttribute EmitContext::GetAttributeInfo(AmdGpu::NumberFormat fmt, Id id,
                                                          u32 num_components, bool output,
                                                          bool loaded, bool array) {
    switch (GetNumberClass(fmt)) {
    case AmdGpu::NumberClass::Float:
        return {id, output ? output_f32 : input_f32, F32[1], num_components, false, loaded, array};
    case AmdGpu::NumberClass::Uint:
        return {id, output ? output_u32 : input_u32, U32[1], num_components, true, loaded, array};
    case AmdGpu::NumberClass::Sint:
        return {id, output ? output_s32 : input_s32, S32[1], num_components, true, loaded, array};
    default:
        break;
    }
    UNREACHABLE_MSG("Invalid attribute type {}", fmt);
}

Id EmitContext::GetBufferSize(const u32 sharp_idx) {
    // Can this be done with memory access? Like we do now with ReadConst
    const auto& srt_flatbuf = buffers[flatbuf_index];
    ASSERT(srt_flatbuf.buffer_type == BufferType::Flatbuf);
    const auto [id, pointer_type] = srt_flatbuf.Alias(PointerType::U32);

    const auto rsrc1{
        OpLoad(U32[1], OpAccessChain(pointer_type, id, u32_zero_value, ConstU32(sharp_idx + 1)))};
    const auto rsrc2{
        OpLoad(U32[1], OpAccessChain(pointer_type, id, u32_zero_value, ConstU32(sharp_idx + 2)))};

    const auto stride{OpBitFieldUExtract(U32[1], rsrc1, ConstU32(16u), ConstU32(14u))};
    const auto num_records{rsrc2};

    const auto stride_zero{OpIEqual(U1[1], stride, u32_zero_value)};
    const auto stride_size{OpIMul(U32[1], num_records, stride)};
    return OpSelect(U32[1], stride_zero, num_records, stride_size);
}

void EmitContext::DefineBufferProperties() {
    if (!profile.needs_buffer_offsets) {
        return;
    }
    for (u32 i = 0; i < buffers.size(); i++) {
        auto& buffer = buffers[i];
        const auto& desc = info.buffers[i];
        const u32 binding = buffer.binding;
        if (buffer.buffer_type != BufferType::Guest) {
            continue;
        }

        const u32 half = PushData::BufOffsetIndex + (binding >> 4);
        const u32 comp = (binding & 0xf) >> 2;
        const u32 offset = (binding & 0x3) << 3;
        const Id ptr{OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U32[1]),
                                   push_data_block, ConstU32(half), ConstU32(comp))};
        const Id value{OpLoad(U32[1], ptr)};

        const Id buf_offset{OpBitFieldUExtract(U32[1], value, ConstU32(offset), ConstU32(8U))};
        Name(buf_offset, fmt::format("buf{}_off", binding));
        buffer.Offset(PointerSize::B8) = buf_offset;

        if (True(desc.used_types & IR::Type::U16)) {
            const Id buf_word_offset{OpShiftRightLogical(U32[1], buf_offset, ConstU32(1U))};
            Name(buf_word_offset, fmt::format("buf{}_word_off", binding));
            buffer.Offset(PointerSize::B16) = buf_word_offset;
        }
        if (True(desc.used_types & IR::Type::U32)) {
            const Id buf_dword_offset{OpShiftRightLogical(U32[1], buf_offset, ConstU32(2U))};
            Name(buf_dword_offset, fmt::format("buf{}_dword_off", binding));
            buffer.Offset(PointerSize::B32) = buf_dword_offset;
        }
        if (True(desc.used_types & IR::Type::U64)) {
            const Id buf_qword_offset{OpShiftRightLogical(U32[1], buf_offset, ConstU32(3U))};
            Name(buf_qword_offset, fmt::format("buf{}_qword_off", binding));
            buffer.Offset(PointerSize::B64) = buf_qword_offset;
        }
    }
}

void EmitContext::DefineAmdPerVertexAttribs() {
    if (!profile.supports_amd_shader_explicit_vertex_parameter) {
        return;
    }
    for (s32 i = 0; i < runtime_info.hw.fs.num_inputs; i++) {
        const auto& input = runtime_info.hw.fs.inputs[i];
        if (input.IsDefault() || info.fs_interpolation[i].primary != Qualifier::PerVertex) {
            continue;
        }
        auto& param = input_params[i];
        const Id pointer = param.id;
        param.id_array[0] =
            OpInterpolateAtVertexAMD(F32[param.num_components], pointer, ConstU32(0U));
        param.id_array[1] =
            OpInterpolateAtVertexAMD(F32[param.num_components], pointer, ConstU32(1U));
        param.id_array[2] =
            OpInterpolateAtVertexAMD(F32[param.num_components], pointer, ConstU32(2U));
        param.is_loaded = true;
    }
}

void EmitContext::DefineWorkgroupIndex() {
    const Id workgroup_id_val{OpLoad(U32[3], workgroup_id)};
    const Id workgroup_x{OpCompositeExtract(U32[1], workgroup_id_val, 0)};
    const Id workgroup_y{OpCompositeExtract(U32[1], workgroup_id_val, 1)};
    const Id workgroup_z{OpCompositeExtract(U32[1], workgroup_id_val, 2)};
    const Id num_workgroups{OpLoad(U32[3], num_workgroups_id)};
    const Id num_workgroups_x{OpCompositeExtract(U32[1], num_workgroups, 0)};
    const Id num_workgroups_y{OpCompositeExtract(U32[1], num_workgroups, 1)};
    workgroup_index_id =
        OpIAdd(U32[1], OpIAdd(U32[1], workgroup_x, OpIMul(U32[1], workgroup_y, num_workgroups_x)),
               OpIMul(U32[1], workgroup_z, OpIMul(U32[1], num_workgroups_x, num_workgroups_y)));
    Name(workgroup_index_id, "workgroup_index");
}

void EmitContext::DefineInputs() {
    if (info.uses_lane_id) {
        subgroup_local_invocation_id = DefineVariable(
            U32[1], spv::BuiltIn::SubgroupLocalInvocationId, spv::StorageClass::Input);
        if (sw_stage != SwStage::Vertex) {
            Decorate(subgroup_local_invocation_id, spv::Decoration::Flat);
        }
    }
    if (info.loads.GetAny(IR::Attribute::SubgroupLtMask)) {
        subgroup_lt_mask =
            DefineVariable(U32[4], spv::BuiltIn::SubgroupLtMask, spv::StorageClass::Input);
        Decorate(subgroup_lt_mask, spv::Decoration::Flat);
    }
    switch (sw_stage) {
    case SwStage::Vertex: {
        vertex_index = DefineVariable(U32[1], spv::BuiltIn::VertexIndex, spv::StorageClass::Input);
        instance_id = DefineVariable(U32[1], spv::BuiltIn::InstanceIndex, spv::StorageClass::Input);
        if (info.loads.Get(IR::Attribute::BaseVertex)) {
            base_vertex =
                DefineVariable(U32[1], spv::BuiltIn::BaseVertex, spv::StorageClass::Input);
        }
        if (info.loads.Get(IR::Attribute::BaseInstance)) {
            base_instance =
                DefineVariable(U32[1], spv::BuiltIn::BaseInstance, spv::StorageClass::Input);
        }

        Shader::Gcn::FetchShaderData fetch_shader;
        if (!Gcn::ParseFetchShader(info, fetch_shader)) {
            break;
        }
        ASSERT(fetch_shader.attributes.size() <= IR::NumParams);
        for (u32 semantic = 0; semantic < fetch_shader.attributes.size(); ++semantic) {
            const auto& attrib = fetch_shader.attributes[semantic];
            const auto sharp = attrib.GetSharp(info);
            const Id type{GetAttributeType(*this, sharp.GetNumberFmt())[4]};
            Id id{DefineInput(type, semantic)};
            if (attrib.GetStepRate() != Gcn::VertexAttribute::InstanceIdType::None) {
                Name(id, fmt::format("vs_instance_attr{}", semantic));
            } else {
                Name(id, fmt::format("vs_in_attr{}", semantic));
            }
            input_params[semantic] = GetAttributeInfo(sharp.GetNumberFmt(), id, 4, false);
        }
        break;
    }
    case SwStage::Fragment: {
        // sotc: Profile::barycentric_via_gs: the barycentrics and per-vertex inputs come from the
        // auxiliary geometry shader as plain inputs (EmitBarycentricGeometryShader).
        const auto gs_layout = MakeBarycentricGsLayout(runtime_info.hw.fs, info);
        std::array<Id, 2> gs_bary{};
        const auto define_bary = [&](bool nopersp) -> Id {
            if (!profile.barycentric_via_gs) {
                return DefineVariable(F32[3],
                                      nopersp ? spv::BuiltIn::BaryCoordNoPerspKHR
                                              : spv::BuiltIn::BaryCoordKHR,
                                      spv::StorageClass::Input);
            }
            Id& id = gs_bary[nopersp ? 1 : 0];
            if (!ValidId(id)) {
                id = DefineInput(F32[3], nopersp ? gs_layout.NoPerspective() : gs_layout.Smooth());
                if (nopersp) {
                    Decorate(id, spv::Decoration::NoPerspective);
                }
            }
            return id;
        };
        if (info.loads.GetAny(IR::Attribute::FragCoord) ||
            (info.loads.GetAny(IR::Attribute::BaryCoordPullModel) &&
             !profile.supports_amd_shader_explicit_vertex_parameter)) {
            frag_coord = DefineVariable(F32[4], spv::BuiltIn::FragCoord, spv::StorageClass::Input);
        }
        if (info.loads.Get(IR::Attribute::IsFrontFace)) {
            front_facing =
                DefineVariable(U1[1], spv::BuiltIn::FrontFacing, spv::StorageClass::Input);
        }
        if (info.loads.GetAny(IR::Attribute::RenderTargetIndex)) {
            output_layer = DefineVariable(U32[1], spv::BuiltIn::Layer, spv::StorageClass::Input);
            Decorate(output_layer, spv::Decoration::Flat);
        }
        if (info.loads.Get(IR::Attribute::SampleIndex)) {
            sample_index = DefineVariable(U32[1], spv::BuiltIn::SampleId, spv::StorageClass::Input);
            Decorate(sample_index, spv::Decoration::Flat);
        }
        if (info.loads.Get(IR::Attribute::IsHelperInvocation)) {
            helper_invocation =
                DefineVariable(U1[1], spv::BuiltIn::HelperInvocation, spv::StorageClass::Input);
        }
        if (info.loads.Get(IR::Attribute::SampleMask)) {
            sample_mask_in = DefineVariable(TypeArray(U32[1], u32_one_value),
                                            spv::BuiltIn::SampleMask, spv::StorageClass::Input);
        }
        if (info.loads.GetAny(IR::Attribute::BaryCoordSmooth)) {
            if (profile.supports_amd_shader_explicit_vertex_parameter) {
                bary_coord_smooth = DefineVariable(F32[2], spv::BuiltIn::BaryCoordSmoothAMD,
                                                   spv::StorageClass::Input);
            } else if (profile.supports_fragment_shader_barycentric && !ValidId(bary_coord)) {
                bary_coord =
                    define_bary(false);
            }
        }
        if (info.loads.GetAny(IR::Attribute::BaryCoordPullModel)) {
            if (profile.supports_amd_shader_explicit_vertex_parameter) {
                bary_coord_pull_model = DefineVariable(F32[3], spv::BuiltIn::BaryCoordPullModelAMD,
                                                       spv::StorageClass::Input);
            } else if (profile.supports_fragment_shader_barycentric && !ValidId(bary_coord)) {
                bary_coord =
                    define_bary(false);
            }
        }
        if (info.loads.GetAny(IR::Attribute::BaryCoordSmoothCentroid)) {
            if (profile.supports_amd_shader_explicit_vertex_parameter) {
                bary_coord_smooth_centroid = DefineVariable(
                    F32[2], spv::BuiltIn::BaryCoordSmoothCentroidAMD, spv::StorageClass::Input);
            } else if (profile.supports_fragment_shader_barycentric && !ValidId(bary_coord)) {
                bary_coord =
                    define_bary(false);
            }
        }
        if (info.loads.GetAny(IR::Attribute::BaryCoordSmoothSample)) {
            if (profile.supports_amd_shader_explicit_vertex_parameter) {
                bary_coord_smooth_sample = DefineVariable(
                    F32[2], spv::BuiltIn::BaryCoordSmoothSampleAMD, spv::StorageClass::Input);
            } else if (profile.supports_fragment_shader_barycentric) {
                if (!ValidId(bary_coord)) {
                    bary_coord = define_bary(false);
                }
                // we would need sample_index to interpolate the bary_coord later
                if (!ValidId(sample_index)) {
                    sample_index =
                        DefineVariable(U32[1], spv::BuiltIn::SampleId, spv::StorageClass::Input);
                    Decorate(sample_index, spv::Decoration::Flat);
                }
            }
        }
        if (info.loads.GetAny(IR::Attribute::BaryCoordNoPersp)) {
            if (profile.supports_amd_shader_explicit_vertex_parameter) {
                bary_coord_nopersp = DefineVariable(F32[2], spv::BuiltIn::BaryCoordNoPerspAMD,
                                                    spv::StorageClass::Input);
            } else if (profile.supports_fragment_shader_barycentric) {
                bary_coord_nopersp = define_bary(true);
            }
        }
        if (info.loads.GetAny(IR::Attribute::BaryCoordNoPerspSample)) {
            if (profile.supports_amd_shader_explicit_vertex_parameter) {
                bary_coord_nopersp_sample = DefineVariable(
                    F32[2], spv::BuiltIn::BaryCoordNoPerspSampleAMD, spv::StorageClass::Input);
            } else if (profile.supports_fragment_shader_barycentric) {
                bary_coord_nopersp_sample = define_bary(true);
                // Decorate(bary_coord_nopersp_sample, spv::Decoration::Sample);
            }
        }

        const bool has_clip_distance_inputs = runtime_info.hw.fs.clip_distance_emulation;
        // Clip distances attribute vector is the last in inputs array
        const auto num_inputs = runtime_info.hw.fs.num_inputs - (has_clip_distance_inputs ? 1 : 0);

        for (s32 i = 0; i < num_inputs; i++) {
            const auto& input = runtime_info.hw.fs.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            const IR::Attribute param = IR::Attribute::Param0 + i;
            const auto [primary, auxiliary] = info.fs_interpolation[i];
            // sotc: the geometry shader writes the three vertex values as vec4s.
            const bool gs_per_vertex = primary == Qualifier::PerVertex && profile.barycentric_via_gs;
            const u32 num_components = gs_per_vertex ? 4 : info.loads.NumComponents(param);
            const Id type = F32[num_components];
            const Id attr_id = [&] {
                const auto bind_location = input.param_index + (has_clip_distance_inputs ? 1 : 0);
                if (primary == Qualifier::PerVertex &&
                    profile.supports_fragment_shader_barycentric) {
                    return Name(DefineInput(TypeArray(type, ConstU32(3U)),
                                            gs_per_vertex ? gs_layout.Location(i) : bind_location),
                                fmt::format("fs_in_attr{}_p", i));
                }
                return Name(DefineInput(type, bind_location), fmt::format("fs_in_attr{}", i));
            }();
            if (gs_per_vertex) {
                Decorate(attr_id, spv::Decoration::Flat);
            } else if (primary == Qualifier::PerVertex) {
                Decorate(attr_id, profile.supports_amd_shader_explicit_vertex_parameter
                                      ? spv::Decoration::ExplicitInterpAMD
                                      : spv::Decoration::PerVertexKHR);
            } else if (primary != Qualifier::Smooth) {
                Decorate(attr_id, primary == Qualifier::Flat ? spv::Decoration::Flat
                                                             : spv::Decoration::NoPerspective);
            }
            if (auxiliary != Qualifier::None) {
                Decorate(attr_id, auxiliary == Qualifier::Centroid ? spv::Decoration::Centroid
                                                                   : spv::Decoration::Sample);
            }
            input_params[i] = GetAttributeInfo(AmdGpu::NumberFormat::Float, attr_id, num_components,
                                               false, false, primary == Qualifier::PerVertex);
        }

        if (has_clip_distance_inputs) {
            const auto type = F32[MaxEmulatedClipDistances];
            const auto attr_id = Name(DefineInput(type, 0), fmt::format("cldist_attr{}", 0));
            input_params[num_inputs] = GetAttributeInfo(AmdGpu::NumberFormat::Float, attr_id,
                                                        MaxEmulatedClipDistances, false);
        }
        break;
    }
    case SwStage::Compute:
        if (info.loads.GetAny(IR::Attribute::WorkgroupIndex) ||
            info.loads.GetAny(IR::Attribute::WorkgroupId)) {
            workgroup_id =
                DefineVariable(U32[3], spv::BuiltIn::WorkgroupId, spv::StorageClass::Input);
        }
        if (info.loads.GetAny(IR::Attribute::WorkgroupIndex)) {
            num_workgroups_id =
                DefineVariable(U32[3], spv::BuiltIn::NumWorkgroups, spv::StorageClass::Input);
        }
        if (info.loads.GetAny(IR::Attribute::LocalInvocationId)) {
            local_invocation_id =
                DefineVariable(U32[3], spv::BuiltIn::LocalInvocationId, spv::StorageClass::Input);
        }
        if (info.loads.Get(IR::Attribute::LocalInvocationIndex)) {
            local_invocation_index = DefineVariable(U32[1], spv::BuiltIn::LocalInvocationIndex,
                                                    spv::StorageClass::Input);
        }
        break;
    case SwStage::Geometry: {
        primitive_id = DefineVariable(U32[1], spv::BuiltIn::PrimitiveId, spv::StorageClass::Input);
        const auto gl_per_vertex =
            Name(TypeStruct(F32[4], F32[1], TypeArray(F32[1], ConstU32(1u))), "gl_PerVertex");
        MemberName(gl_per_vertex, 0, "gl_Position");
        MemberName(gl_per_vertex, 1, "gl_PointSize");
        MemberName(gl_per_vertex, 2, "gl_ClipDistance");
        MemberDecorate(gl_per_vertex, 0, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::Position));
        MemberDecorate(gl_per_vertex, 1, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::PointSize));
        MemberDecorate(gl_per_vertex, 2, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::ClipDistance));
        Decorate(gl_per_vertex, spv::Decoration::Block);
        const auto num_verts_in = NumVertices(runtime_info.hw.gs.in_primitive);
        const auto vertices_in = TypeArray(gl_per_vertex, ConstU32(num_verts_in));
        gl_in = Name(DefineVar(vertices_in, spv::StorageClass::Input), "gl_in");
        interfaces.push_back(gl_in);

        const auto num_params = runtime_info.hw.gs.in_vertex_data_size / 4 - 1u;
        for (int param_id = 0; param_id < num_params; ++param_id) {
            const Id type{TypeArray(F32[4], ConstU32(num_verts_in))};
            const Id id{DefineInput(type, param_id)};
            Name(id, fmt::format("gs_in_attr{}", param_id));
            input_params[param_id] =
                GetAttributeInfo(AmdGpu::NumberFormat::Float, id, 4, false, false, true);
        }
        break;
    }
    case SwStage::TessellationControl: {
        invocation_id =
            DefineVariable(U32[1], spv::BuiltIn::InvocationId, spv::StorageClass::Input);
        patch_vertices =
            DefineVariable(U32[1], spv::BuiltIn::PatchVertices, spv::StorageClass::Input);
        primitive_id = DefineVariable(U32[1], spv::BuiltIn::PrimitiveId, spv::StorageClass::Input);

        const u32 num_attrs = Common::AlignUp(runtime_info.sw.tcs.ls_stride, 16) >> 4;
        if (num_attrs > 0) {
            const Id per_vertex_type{TypeArray(F32[4], ConstU32(num_attrs))};
            // The input vertex count isn't statically known, so make length 32 (what
            // glslang does)
            const Id patch_array_type{TypeArray(per_vertex_type, ConstU32(32u))};
            input_attr_array = DefineInput(patch_array_type, 0);
            Name(input_attr_array, "in_attrs");
        }
        break;
    }
    case SwStage::TessellationEval: {
        tess_coord = DefineInput(F32[3], std::nullopt, spv::BuiltIn::TessCoord);
        primitive_id = DefineVariable(U32[1], spv::BuiltIn::PrimitiveId, spv::StorageClass::Input);

        const u32 num_attrs = Common::AlignUp(runtime_info.sw.tes.hs_output_cp_stride, 16) >> 4;
        if (num_attrs > 0) {
            const Id per_vertex_type{TypeArray(F32[4], ConstU32(num_attrs))};
            // The input vertex count isn't statically known, so make length 32 (what
            // glslang does)
            const Id patch_array_type{TypeArray(per_vertex_type, ConstU32(32u))};
            input_attr_array = DefineInput(patch_array_type, 0);
            Name(input_attr_array, "in_attrs");
        }

        const u32 patch_base_location = num_attrs;
        for (size_t index = 0; index < 30; ++index) {
            if (!(info.uses_patches & (1U << index))) {
                continue;
            }
            const Id id{DefineInput(F32[4], patch_base_location + index)};
            Decorate(id, spv::Decoration::Patch);
            Name(id, fmt::format("patch_in{}", index));
            patches[index] = id;
        }
        break;
    }
    default:
        break;
    }
}

void EmitContext::DefineVertexBlock() {
    const std::array<Id, 8> zero{f32_zero_value, f32_zero_value, f32_zero_value, f32_zero_value,
                                 f32_zero_value, f32_zero_value, f32_zero_value, f32_zero_value};
    output_position = DefineVariable(F32[4], spv::BuiltIn::Position, spv::StorageClass::Output);
    // sotc: barycentric emulation adds a geometry stage to some pipelines only (profile.h,
    // barycentric_via_gs): the same vertex shader must give the same position with and without
    // it, since the depth pre-pass and colour pass can pick different pipelines and still need
    // an equal depth test. Without this, ground-level surfaces can show as untextured or black
    // on GPUs that take this path (no VK_KHR_fragment_shader_barycentric /
    // VK_AMD_shader_explicit_vertex_parameter).
    if (profile.barycentric_via_gs) {
        Decorate(output_position, spv::Decoration::Invariant);
    }
    const bool needs_clip_distance_emulation = sw_stage == SwStage::Vertex &&
                                               hw_stage == HwStage::Vertex &&
                                               profile.needs_clip_distance_emulation;
    const auto has_clip_distance_outputs = info.stores.GetAny(IR::Attribute::ClipDistance);
    if (has_clip_distance_outputs && !needs_clip_distance_emulation) {
        const Id type{TypeArray(F32[1], ConstU32(8U))};
        const Id initializer{ConstantComposite(type, zero)};
        clip_distances = DefineVariable(type, spv::BuiltIn::ClipDistance, spv::StorageClass::Output,
                                        initializer);
    }
    if (info.stores.GetAny(IR::Attribute::CullDistance)) {
        const Id type{TypeArray(F32[1], ConstU32(8U))};
        const Id initializer{ConstantComposite(type, zero)};
        cull_distances = DefineVariable(type, spv::BuiltIn::CullDistance, spv::StorageClass::Output,
                                        initializer);
    }
    if (info.stores.GetAny(IR::Attribute::PointSize)) {
        output_point_size =
            DefineVariable(F32[1], spv::BuiltIn::PointSize, spv::StorageClass::Output);
    }
    if (info.stores.GetAny(IR::Attribute::RenderTargetIndex)) {
        output_layer = DefineVariable(U32[1], spv::BuiltIn::Layer, spv::StorageClass::Output);
    }
    if (info.stores.GetAny(IR::Attribute::ViewportIndex)) {
        output_viewport_index =
            DefineVariable(U32[1], spv::BuiltIn::ViewportIndex, spv::StorageClass::Output);
    }
}

void EmitContext::DefineOutputs() {
    switch (sw_stage) {
    case SwStage::Vertex: {
        DefineVertexBlock();
        if (hw_stage == Shader::HwStage::Local) {
            const u32 num_attrs = Common::AlignUp(runtime_info.hw.ls.ls_stride, 16) >> 4;
            if (num_attrs > 0) {
                const Id type{TypeArray(F32[4], ConstU32(num_attrs))};
                output_attr_array = DefineOutput(type, 0);
                Name(output_attr_array, "out_attrs");
            }
        } else {
            const bool needs_clip_distance_emulation =
                hw_stage == HwStage::Vertex && profile.needs_clip_distance_emulation &&
                info.stores.GetAny(IR::Attribute::ClipDistance);
            u32 num_attrs = 0u;
            for (u32 i = 0; i < IR::NumParams; i++) {
                const IR::Attribute param{IR::Attribute::Param0 + i};
                if (!info.stores.GetAny(param)) {
                    continue;
                }
                const u32 num_components = info.stores.NumComponents(param);
                const Id id{
                    DefineOutput(F32[num_components], i + (needs_clip_distance_emulation ? 1 : 0))};
                Name(id, fmt::format("out_attr{}", i));
                output_params[i] =
                    GetAttributeInfo(AmdGpu::NumberFormat::Float, id, num_components, true);
                ++num_attrs;
            }

            if (needs_clip_distance_emulation) {
                clip_distances = Id{DefineOutput(F32[MaxEmulatedClipDistances], 0)};
                output_params[num_attrs] = GetAttributeInfo(
                    AmdGpu::NumberFormat::Float, clip_distances, MaxEmulatedClipDistances, true);
                Name(clip_distances, fmt::format("cldist_attr{}", 0));
            }
        }
        break;
    }
    case SwStage::TessellationControl: {
        if (info.stores_tess_level_outer) {
            const Id type{TypeArray(F32[1], ConstU32(4U))};
            output_tess_level_outer =
                DefineOutput(type, std::nullopt, spv::BuiltIn::TessLevelOuter);
            Decorate(output_tess_level_outer, spv::Decoration::Patch);
        }
        if (info.stores_tess_level_inner) {
            const Id type{TypeArray(F32[1], ConstU32(2U))};
            output_tess_level_inner =
                DefineOutput(type, std::nullopt, spv::BuiltIn::TessLevelInner);
            Decorate(output_tess_level_inner, spv::Decoration::Patch);
        }

        const u32 num_attrs = Common::AlignUp(runtime_info.sw.tcs.hs_output_cp_stride, 16) >> 4;
        if (num_attrs > 0) {
            const Id per_vertex_type{TypeArray(F32[4], ConstU32(num_attrs))};
            // The input vertex count isn't statically known, so make length 32 (what
            // glslang does)
            const Id patch_array_type{
                TypeArray(per_vertex_type, ConstU32(runtime_info.sw.tcs.NumOutputControlPoints()))};
            output_attr_array = DefineOutput(patch_array_type, 0);
            Name(output_attr_array, "out_attrs");
        }

        const u32 patch_base_location = num_attrs;
        for (size_t index = 0; index < 30; ++index) {
            if (!(info.uses_patches & (1U << index))) {
                continue;
            }
            const Id id{DefineOutput(F32[4], patch_base_location + index)};
            Decorate(id, spv::Decoration::Patch);
            Name(id, fmt::format("patch_out{}", index));
            patches[index] = id;
        }
        break;
    }
    case SwStage::TessellationEval: {
        DefineVertexBlock();
        for (u32 i = 0; i < IR::NumParams; i++) {
            const IR::Attribute param{IR::Attribute::Param0 + i};
            if (!info.stores.GetAny(param)) {
                continue;
            }
            const u32 num_components = info.stores.NumComponents(param);
            const Id id{DefineOutput(F32[num_components], i)};
            Name(id, fmt::format("out_attr{}", i));
            output_params[i] =
                GetAttributeInfo(AmdGpu::NumberFormat::Float, id, num_components, true);
        }
        break;
    }
    case SwStage::Fragment: {
        if (info.stores.Get(IR::Attribute::Depth)) {
            frag_depth = DefineVariable(F32[1], spv::BuiltIn::FragDepth, spv::StorageClass::Output);
        }
        if (info.stores.Get(IR::Attribute::SampleMask)) {
            sample_mask = DefineVariable(TypeArray(U32[1], u32_one_value), spv::BuiltIn::SampleMask,
                                         spv::StorageClass::Output);
        }
        if (info.stores.Get(IR::Attribute::StencilRef) && profile.supports_shader_stencil_export) {
            stencil_ref =
                DefineVariable(S32[1], spv::BuiltIn::FragStencilRefEXT, spv::StorageClass::Output);
        }
        u32 num_render_targets = 0;
        for (u32 i = 0; i < IR::NumRenderTargets; i++) {
            const IR::Attribute mrt{IR::Attribute::RenderTarget0 + i};
            if (!info.stores.GetAny(mrt)) {
                continue;
            }
            const u32 num_components = info.stores.NumComponents(mrt);
            const AmdGpu::NumberFormat num_format{runtime_info.hw.fs.color_buffers[i].num_format};
            const Id type{GetAttributeType(*this, num_format)[num_components]};
            Id id;
            if (runtime_info.hw.fs.dual_source_blending) {
                id = DefineOutput(type, 0);
                Decorate(id, spv::Decoration::Index, i);
            } else {
                id = DefineOutput(type, i);
            }
            Name(id, fmt::format("frag_color{}", i));
            frag_outputs[i] = GetAttributeInfo(num_format, id, num_components, true);
            ++num_render_targets;
        }
        // Dual source blending allows at most 2 render targets, one for each source.
        // Fewer targets are allowed but the missing blending source values will be
        // undefined.
        ASSERT_MSG(!runtime_info.hw.fs.dual_source_blending || num_render_targets <= 2,
                   "Dual source blending enabled, there must be at most two MRT exports");
        break;
    }
    case SwStage::Geometry: {
        DefineVertexBlock();
        for (u32 attr_id = 0; attr_id < info.gs_copy_data.num_attrs; attr_id++) {
            const Id id{DefineOutput(F32[4], attr_id)};
            Name(id, fmt::format("out_attr{}", attr_id));
            output_params[attr_id] = GetAttributeInfo(AmdGpu::NumberFormat::Float, id, 4, true);
        }
        break;
    }
    case SwStage::Compute:
        break;
    default:
        UNREACHABLE();
    }
}

void EmitContext::DefineHeapPushDataBlock() {
    // sotc: SOTC_HEAP. A heap stage reads no sampler or image words: its indices come from the index table. The
    // struct is PushData, then the table address (member 7 at 64, where DrawPacket::index_table_address is), the debug
    // buffer address (member 8 at 128), and in GPU verify the 14 classic words at 136 (resource.h). The old layout put
    // the table address at 56 (the sampler word 0), so a heap stage read the packet's flatbuf address as its table.
    const bool verify = profile.heap_images == 2;
    // sotc: SOTC_HEAP_VS. A vertex heap stage has its table address at member 9 (byte 136); GPU verify never has one.
    const bool vs_table = !verify && info.sw_stage == Shader::SwStage::Vertex;
    // sotc: SOTC_GCN_TEX. With gcn_tex the globals address is member 8 (byte 88) and the members after it move by one.
    const u32 shift = HeapMemberShift(profile);
    std::vector<Id> members{F32[1], F32[1], F32[1], F32[1], U32[4], U32[4], U32[2]};
    members.push_back(U64); // member 7: the table address
    if (shift) {
        members.push_back(U64); // member 8: the globals address (gcn_layout.h)
    }
    members.push_back(U64); // member 8 + shift: the debug buffer address
    if (verify) {
        for (u32 word = 0; word < Shader::HeapVerifySamplerWords + Shader::HeapVerifyImageWords; ++word) {
            members.push_back(U32[1]); // members 9 + shift..: the classic words
        }
    } else if (vs_table) {
        members.push_back(U64); // member 9 + shift: the vertex heap stage's table address
    }
    const Id struct_type{Name(TypeStruct(std::span<const Id>(members)), "AuxData")};
    Decorate(struct_type, spv::Decoration::Block);
    MemberDecorate(struct_type, PushData::XOffsetIndex, spv::Decoration::Offset, 0U);
    MemberDecorate(struct_type, PushData::YOffsetIndex, spv::Decoration::Offset, 4U);
    MemberDecorate(struct_type, PushData::XScaleIndex, spv::Decoration::Offset, 8U);
    MemberDecorate(struct_type, PushData::YScaleIndex, spv::Decoration::Offset, 12U);
    MemberDecorate(struct_type, PushData::BufOffsetIndex + 0, spv::Decoration::Offset, 16U);
    MemberDecorate(struct_type, PushData::BufOffsetIndex + 1, spv::Decoration::Offset, 32U);
    MemberDecorate(struct_type, PushData::BufOffsetIndex + 2, spv::Decoration::Offset, 48U);
    MemberDecorate(struct_type, HeapIndexTableMember, spv::Decoration::Offset, HeapIndexTableOffset);
    if (shift) {
        MemberDecorate(struct_type, HeapGcnGlobalsMember, spv::Decoration::Offset, HeapGlobalsOffset);
    }
    MemberDecorate(struct_type, HeapDebugMember + shift, spv::Decoration::Offset, Shader::HeapDebugOffset);
    if (verify) {
        for (u32 word = 0; word < Shader::HeapVerifySamplerWords + Shader::HeapVerifyImageWords; ++word) {
            MemberDecorate(struct_type, HeapVerifyFirstMember + shift + word, spv::Decoration::Offset,
                           Shader::HeapVerifyWordsOffset + 4 * word);
        }
    } else if (vs_table) {
        MemberDecorate(struct_type, HeapVsTableMember + shift, spv::Decoration::Offset, Shader::HeapVsTableOffset);
    }
    push_data_block = DefineVar(struct_type, spv::StorageClass::PushConstant);
    Name(push_data_block, "push_data");
    interfaces.push_back(push_data_block);
}

void EmitContext::DefinePushDataBlock() {
    // sotc: SOTC_HEAP: the heap stage's own push struct (see DefineHeapPushDataBlock).
    if (UsesHeapImages(info, profile)) {
        DefineHeapPushDataBlock();
        return;
    }
    // Create push constants block for instance steps rates
    // sotc: a bindless stage also reads the sampler slots, pushed right after PushData
    // (BindlessSamplerSlots). Other stages keep the block as it was.
    // sotc: SOTC_N5_BINDLESS: the image words follow the sampler words. A stage with bindless images
    // and no bindless sampler gets the sampler words as padding, so the members stay in order.
    const bool bindless_images = UsesBindlessImages(info);
    const bool bindless = UsesBindlessSamplers(info) || bindless_images;
    std::vector<Id> members{F32[1], F32[1], F32[1], F32[1], U32[4], U32[4], U32[2]};
    if (bindless) {
        for (u32 word = 0; word < NUM_BINDLESS_SAMPLER_WORDS; ++word) {
            members.push_back(U32[1]);
        }
    }
    if (bindless_images) {
        for (u32 word = 0; word < NUM_BINDLESS_IMAGE_WORDS; ++word) {
            members.push_back(U32[1]);
        }
    }
    // sotc: SOTC_SHADER_ADDR: the window words of the packet (ShaderAddr*Offset, 72..120).
    const bool shader_addr = UsesShaderAddr(profile, info);
    // sotc: SOTC_SHADER_ADDR=2: the debug buffer address at byte 64 (ShaderAddrVerifyDebugOffset). A stage with
    // shader-addressed buffers has no bindless words, so the members here are the 7 of PushData (56 bytes).
    const bool verify = shader_addr && ShaderAddrVerifyMode(profile);
    if (verify) {
        shader_addr_debug_member = static_cast<u32>(members.size());
        members.push_back(U64);
    }
    if (shader_addr) {
        shader_addr_member = static_cast<u32>(members.size());
        for (u32 word = 0; word < 6; ++word) {
            members.push_back(U64);
        }
    }
    const Id struct_type{Name(TypeStruct(std::span<const Id>(members)), "AuxData")};
    Decorate(struct_type, spv::Decoration::Block);
    MemberName(struct_type, PushData::XOffsetIndex, "xoffset");
    MemberName(struct_type, PushData::YOffsetIndex, "yoffset");
    MemberName(struct_type, PushData::XScaleIndex, "xscale");
    MemberName(struct_type, PushData::YScaleIndex, "yscale");
    MemberName(struct_type, PushData::BufOffsetIndex + 0, "buf_offsets0");
    MemberName(struct_type, PushData::BufOffsetIndex + 1, "buf_offsets1");
    MemberName(struct_type, PushData::BufOffsetIndex + 2, "buf_offsets2");
    MemberDecorate(struct_type, PushData::XOffsetIndex, spv::Decoration::Offset, 0U);
    MemberDecorate(struct_type, PushData::YOffsetIndex, spv::Decoration::Offset, 4U);
    MemberDecorate(struct_type, PushData::XScaleIndex, spv::Decoration::Offset, 8U);
    MemberDecorate(struct_type, PushData::YScaleIndex, spv::Decoration::Offset, 12U);
    MemberDecorate(struct_type, PushData::BufOffsetIndex + 0, spv::Decoration::Offset, 16U);
    MemberDecorate(struct_type, PushData::BufOffsetIndex + 1, spv::Decoration::Offset, 32U);
    MemberDecorate(struct_type, PushData::BufOffsetIndex + 2, spv::Decoration::Offset, 48U);
    if (bindless) {
        for (u32 word = 0; word < NUM_BINDLESS_SAMPLER_WORDS; ++word) {
            MemberDecorate(struct_type, PushData::SamplerSlotIndex + word,
                           spv::Decoration::Offset,
                           static_cast<u32>(sizeof(PushData) + word * sizeof(u32)));
        }
    }
    if (bindless_images) {
        for (u32 word = 0; word < NUM_BINDLESS_IMAGE_WORDS; ++word) {
            MemberDecorate(struct_type, PushData::ImageSlotIndex + word, spv::Decoration::Offset,
                           static_cast<u32>(sizeof(PushData) +
                                            (NUM_BINDLESS_SAMPLER_WORDS + word) * sizeof(u32)));
        }
    }
    if (shader_addr) {
        const std::array<u32, 6> window_offsets{
            ShaderAddrMirrorGuestOffset, ShaderAddrMirrorDeviceOffset, ShaderAddrArenaGuestOffset,
            ShaderAddrArenaDeviceOffset, ShaderAddrArenaSizeOffset,    ShaderAddrMirrorSizeOffset};
        for (u32 member = 0; member < window_offsets.size(); ++member) {
            MemberDecorate(struct_type, shader_addr_member + member, spv::Decoration::Offset,
                           window_offsets[member]);
        }
    }
    if (verify) {
        MemberDecorate(struct_type, shader_addr_debug_member, spv::Decoration::Offset,
                       ShaderAddrVerifyDebugOffset);
    }
    push_data_block = DefineVar(struct_type, spv::StorageClass::PushConstant);
    Name(push_data_block, "push_data");
    interfaces.push_back(push_data_block);
}

EmitContext::BufferSpv EmitContext::DefineBuffer(bool is_written, bool is_coherent, u32 elem_shift,
                                                 BufferType buffer_type, Id data_type) {
    // Define array type.
    const Id record_array_type{TypeRuntimeArray(data_type)};
    // Define block struct type. Don't perform decorations twice on the same Id.
    const Id struct_type{TypeStruct(record_array_type)};
    if (std::ranges::find(buf_type_ids, record_array_type.value, &Id::value) ==
        buf_type_ids.end()) {
        Decorate(record_array_type, spv::Decoration::ArrayStride, 1 << elem_shift);
        Decorate(struct_type, spv::Decoration::Block);
        MemberName(struct_type, 0, "data");
        MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);
        buf_type_ids.push_back(record_array_type);
    }
    // Define buffer binding interface.
    constexpr auto storage_class = spv::StorageClass::StorageBuffer;
    const Id struct_pointer_type{TypePointer(storage_class, struct_type)};
    const Id pointer_type = TypePointer(storage_class, data_type);
    const Id id{AddGlobalVariable(struct_pointer_type, storage_class)};
    Decorate(id, spv::Decoration::Binding, binding.unified);
    Decorate(id, spv::Decoration::DescriptorSet, DescriptorSetIndex());
    if (!is_written) {
        Decorate(id, spv::Decoration::NonWritable);
    } else if (is_coherent) {
        // The guest V# MTYPE marks this buffer as shared between invocations;
        // Coherent forbids caching its accesses in registers across barriers.
        Decorate(id, spv::Decoration::Coherent);
    }
    switch (buffer_type) {
    case BufferType::GdsBuffer:
        Name(id, "gds_buffer");
        break;
    case BufferType::Flatbuf:
        Name(id, "srt_flatbuf");
        break;
    case BufferType::ClipPlanes:
        Name(id, "clip_planes");
        break;
    case BufferType::BdaPagetable:
        Name(id, "bda_pagetable");
        break;
    case BufferType::FaultBuffer:
        Name(id, "fault_buffer");
        break;
    case BufferType::LodFeedback:
        Name(id, "lod_feedback");
        break;
    case BufferType::BdaBlockTable:
        Name(id, "bda_block_table");
        break;
    case BufferType::SharedMemory:
        Name(id, "ssbo_shmem");
        break;
    default:
        Name(id, fmt::format("ssbo_{}", binding.buffer));
        break;
    }
    interfaces.push_back(id);
    return {id, pointer_type};
};

std::pair<Id, Id> EmitContext::LoadBdaBlock(u32 handle) {
    if (buffers[handle].shader_addr) { // sotc: SOTC_SHADER_ADDR: the block comes from the V#
        return LoadShaderAddrBlock(handle);
    }
    const auto [table_id, table_pointer] = buffers[bda_table_index].Alias(PointerType::U32);
    const auto word = [&](u32 index) {
        const Id ptr{OpAccessChain(table_pointer, table_id, u32_zero_value,
                                   ConstU32(handle * 4 + index))};
        return OpLoad(U32[1], ptr);
    };
    const Id low{word(0)};
    const Id high{word(1)};
    const Id size{word(2)};
    const Id address{OpBitcast(U64, OpCompositeConstruct(U32[2], std::array<Id, 2>{low, high}))};
    return {address, size};
}

// sotc: SOTC_SHADER_ADDR. Mirrors AmdGpu::Buffer exactly: base = dw0 | (dw1 & 0xff) << 32 (base_address
// is 40 bits), stride = dw1 bits 16..29, num_records = dw2, type = dw3 bits 30..31 (Valid() needs 0), and
// GetSize = stride == 0 ? num_records : stride * num_records in u32. Window check: base must lie in the arena
// window, else in mirror window 0 (push block words); the size is clipped to that window's end.
std::pair<Id, Id> EmitContext::LoadShaderAddrBlock(u32 handle) {
    const auto& desc = info.buffers[handle];
    const BufferSpv flat = buffers[flatbuf_index].Alias(PointerType::U32);
    // The words SharpFetch::Fetch reads: contiguous for SingleLoad, per dword (an immediate where the
    // load mask is clear) for MultiLoad. Invalid sharps are excluded by ShaderAddrBuffer.
    const auto dword = [&](u32 i) -> Id {
        const auto& fetch = desc.sharp_fetch;
        if (fetch.summary == SharpFetch<AmdGpu::Buffer>::Summary::SingleLoad) {
            const u32 index = u32{fetch.offsets[0]} + i;
            return OpLoad(U32[1], OpAccessChain(flat.pointer_type, flat.id, u32_zero_value,
                                                ConstU32(index)));
        }
        if ((fetch.load_mask >> i) & 1) {
            return OpLoad(U32[1], OpAccessChain(flat.pointer_type, flat.id, u32_zero_value,
                                                ConstU32(u32{fetch.offsets[i]})));
        }
        return ConstU32(fetch.immediates[i]);
    };
    const Id dw0{dword(0)};
    const Id dw1{dword(1)};
    const Id dw2{dword(2)};
    const Id dw3{dword(3)};

    // AmdGpu::Buffer::Valid(): type (dw3 bits 30..31) is 0. An invalid V# is a null buffer (size 0).
    const Id type{OpShiftRightLogical(U32[1], dw3, ConstU32(30u))};
    const Id is_valid{OpIEqual(U1[1], type, u32_zero_value)};

    // GetSize: u32 arithmetic, wrapping like the CPU product.
    const Id stride{OpBitFieldUExtract(U32[1], dw1, ConstU32(16u), ConstU32(14u))};
    const Id stride_zero{OpIEqual(U1[1], stride, u32_zero_value)};
    const Id stride_size{OpIMul(U32[1], dw2, stride)};
    const Id size{OpSelect(U32[1], stride_zero, dw2, stride_size)};

    // base_address: 40 bits, dw0 and the low byte of dw1.
    const Id base_hi{OpShiftLeftLogical(
        U64, OpUConvert(U64, OpBitFieldUExtract(U32[1], dw1, ConstU32(0u), ConstU32(8u))),
        Constant(U64, 32ULL))};
    const Id base{OpIAdd(U64, OpUConvert(U64, dw0), base_hi)};
    const Id size64{OpUConvert(U64, size)};

    const auto push_u64 = [&](u32 member) {
        return OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64),
                                         push_data_block, ConstU32(shader_addr_member + member)));
    };
    // Window words, in the order of the window offsets (resource.h).
    const Id mirror_guest{push_u64(0)};
    const Id mirror_device{push_u64(1)};
    const Id arena_guest{push_u64(2)};
    const Id arena_device{push_u64(3)};
    const Id arena_size{push_u64(4)};
    const Id mirror_size{push_u64(5)};

    // sotc: SOTC_ADDR_TABLE (table modes). The block is the guest range itself, clipped to the table window
    // (mirror_guest, mirror_size); TableAddresses translates each access. An invalid V# or a base outside the window
    // gives size 0, so the access is invalid.
    if (ShaderAddrTableMode(profile)) {
        const Id table_rel{OpISub(U64, base, mirror_guest)};
        const Id table_in{OpLogicalAnd(U1[1], OpUGreaterThanEqual(U1[1], base, mirror_guest),
                                       OpULessThan(U1[1], table_rel, mirror_size))};
        const Id table_room{OpISub(U64, mirror_size, table_rel)};
        const Id table_bytes{OpSelect(U64, OpULessThanEqual(U1[1], size64, table_room), size64, table_room)};
        const Id table_valid{OpLogicalAnd(U1[1], is_valid, table_in)};
        return {base, OpSelect(U32[1], table_valid, OpUConvert(U32[1], table_bytes), u32_zero_value)};
    }

    // sotc: SOTC_SHADER_ADDR. [guest, guest + window_size) holds base (no wrap): rel is the offset of base in the
    // window. Only the start must lie in the window; a range that runs past its end is clipped below.
    const auto holds = [&](Id guest, Id window_size) {
        const Id from_start{OpUGreaterThanEqual(U1[1], base, guest)};
        const Id rel{OpISub(U64, base, guest)};
        const Id inside{OpULessThan(U1[1], rel, window_size)};
        return std::pair{rel, OpLogicalAnd(U1[1], from_start, inside)};
    };
    // sotc: the bytes of [base, base + size) that lie in the window, min(size, window_size - rel) (u64). The classic
    // descriptor is clamped to the mapped memory the same way (ClampRangeSize in BindBuffers), so a V# whose
    // num_records runs past the window keeps the bytes that are inside it instead of reading zero everywhere.
    const auto clipped = [&](Id rel, Id window_size) {
        const Id room{OpISub(U64, window_size, rel)};
        const Id fits_size{OpULessThanEqual(U1[1], size64, room)};
        return OpSelect(U64, fits_size, size64, room);
    };
    // Arena first, then mirror window 0 (the same order as the host packet).
    const auto [arena_rel, in_arena]{holds(arena_guest, arena_size)};
    const auto [mirror_rel, in_mirror]{holds(mirror_guest, mirror_size)};
    const Id arena_address{OpIAdd(U64, arena_device, arena_rel)};
    const Id mirror_address{OpIAdd(U64, mirror_device, mirror_rel)};
    const Id address{OpSelect(U64, in_arena, arena_address, mirror_address)};
    const Id arena_bytes{clipped(arena_rel, arena_size)};
    const Id mirror_bytes{clipped(mirror_rel, mirror_size)};
    const Id bytes{OpSelect(U64, in_arena, arena_bytes, mirror_bytes)};

    // Base outside both windows or invalid V#: size 0, so BdaAddress reports the access invalid.
    const Id in_window{OpLogicalOr(U1[1], in_arena, in_mirror)};
    const Id valid{OpLogicalAnd(U1[1], is_valid, in_window)};
    const Id block_size{OpSelect(U32[1], valid, OpUConvert(U32[1], bytes), u32_zero_value)};
    return {address, block_size};
}

// sotc: SOTC_SHADER_ADDR=2. The guest base of the V# of buffer `handle` (dw0 | (dw1 & 0xff) << 32, as in
// LoadShaderAddrBlock). Only the debug record reads it.
Id EmitContext::LoadShaderAddrGuestBase(u32 handle) {
    const auto& desc = info.buffers[handle];
    const BufferSpv flat = buffers[flatbuf_index].Alias(PointerType::U32);
    const auto dword = [&](u32 i) -> Id {
        const auto& fetch = desc.sharp_fetch;
        if (fetch.summary == SharpFetch<AmdGpu::Buffer>::Summary::SingleLoad) {
            const u32 index = u32{fetch.offsets[0]} + i;
            return OpLoad(U32[1], OpAccessChain(flat.pointer_type, flat.id, u32_zero_value,
                                                ConstU32(index)));
        }
        if ((fetch.load_mask >> i) & 1) {
            return OpLoad(U32[1], OpAccessChain(flat.pointer_type, flat.id, u32_zero_value,
                                                ConstU32(u32{fetch.offsets[i]})));
        }
        return ConstU32(fetch.immediates[i]);
    };
    const Id dw0{dword(0)};
    const Id dw1{dword(1)};
    const Id base_hi{OpShiftLeftLogical(
        U64, OpUConvert(U64, OpBitFieldUExtract(U32[1], dw1, ConstU32(0u), ConstU32(8u))),
        Constant(U64, 32ULL))};
    return OpIAdd(U64, OpUConvert(U64, dw0), base_hi);
}

// sotc: SOTC_SHADER_ADDR=2 (verify). The loads of a shader-addressed buffer. Two reads of the same bytes:
//   1. the descriptor (the classic path: buffer offset, alias, one access chain per element). Its value is the
//      result, so rendering is the same as in mode 0; it is read whatever the address says.
//   2. the shader address (BdaAddress, the same as mode 1). Only when all `count` elements lie in the block
//      (valid): each lane is loaded and compared bitwise with its descriptor value. A differing lane appends one
//      record to the debug buffer (push member shader_addr_debug_member; layout in resource.h).
// The value of the address read never leaves its block, so no OpPhi is needed: the result is defined before the
// first branch. Every counter and record write is an atomic on a physical pointer (emit_spirv_atomic.cpp, scope
// Device, no semantics). An access outside the block counts as uncompared.
Id EmitContext::ShaderAddrVerifyLoad(u32 handle, Id index, u32 elem_size, PointerType alias,
                                     PointerSize offset_size, Id elem_type, Id result_type,
                                     u32 count) {
    const auto& spv_buffer = buffers[handle];
    const auto [alias_id, pointer_type] = spv_buffer.Alias(alias);
    const Id zero = alias == PointerType::U64 ? u64_zero_value : u32_zero_value;
    Id desc_index{index};
    if (const Id offset = spv_buffer.Offset(offset_size); Sirit::ValidId(offset)) {
        desc_index = OpIAdd(U32[1], index, offset);
    }
    boost::container::static_vector<Id, 4> desc_values;
    for (u32 i = 0; i < count; i++) {
        const Id index_i = i == 0 ? desc_index : OpIAdd(U32[1], desc_index, ConstU32(i));
        const Id ptr_i = OpAccessChain(pointer_type, alias_id, zero, index_i);
        desc_values.push_back(OpLoad(elem_type, ptr_i));
    }

    // The address read: the same (address, valid) as a BDA load, and the block size and guest base for the record.
    // sotc: SOTC_ADDR_TABLE (table modes): every element is translated (TableAddresses), not address + i * size.
    const bool table = handle < buffers.size() && buffers[handle].shader_addr && ShaderAddrTableMode(profile);
    boost::container::static_vector<Id, 4> table_addresses;
    Id address{};
    Id valid{};
    if (table) {
        valid = TableAddresses(handle, index, elem_size, count, table_addresses);
        address = table_addresses[0];
    } else {
        const auto [first, ok] = BdaAddress(handle, index, elem_size, count);
        address = first;
        valid = ok;
    }
    const Id block_size{LoadBdaBlock(handle).second};
    const Id guest_base{LoadShaderAddrGuestBase(handle)};
    const Id debug_base{OpLoad(
        U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64), push_data_block,
                           ConstU32(shader_addr_debug_member)))};
    const auto debug_word = [&](u32 offset) {
        return BdaPointer(OpIAdd(U64, debug_base, Constant(U64, static_cast<u64>(offset))), U32[1]);
    };
    const auto add_word = [&](u32 offset, Id value) {
        const Id scope{ConstU32(static_cast<u32>(spv::Scope::Device))};
        return OpAtomicIAdd(U32[1], debug_word(offset), scope, u32_zero_value, value);
    };
    const auto bits = [&](Id value) -> Id {
        switch (alias) {
        case PointerType::F32:
            return OpUConvert(U64, OpBitcast(U32[1], value));
        case PointerType::U64:
            return value;
        default:
            return OpUConvert(U64, value);
        }
    };

    // Outside the block (the descriptor read is 0 there under robustness): not compared.
    const Id uncompared_merge = BdaBeginIf(OpLogicalNot(U1[1], valid));
    add_word(ShaderAddrVerifyUncompared, ConstU32(1u));
    // An address-path load returns 0 here: a non-zero descriptor value is a real divergence of mode 1. It is counted
    // and recorded with device address ~0 (the access was invalid, no address was read).
    for (u32 i = 0; i < count; i++) {
        const Id desc_bits{bits(desc_values[i])};
        const Id nonzero_merge = BdaBeginIf(OpINotEqual(U1[1], desc_bits, u64_zero_value));
        add_word(ShaderAddrVerifyUncomparedNonZero, ConstU32(1u));
        const Id slot{add_word(ShaderAddrVerifySlots, ConstU32(1u))};
        const Id fits{OpULessThan(U1[1], slot, ConstU32(ShaderAddrVerifyCapacity))};
        const Id record_merge = BdaBeginIf(fits);
        const Id record{OpIAdd(
            U64, debug_base,
            OpIAdd(U64,
                   OpIMul(U64, OpUConvert(U64, slot),
                          Constant(U64, static_cast<u64>(ShaderAddrVerifyRecordSize))),
                   Constant(U64, static_cast<u64>(ShaderAddrVerifyRecordsOffset))))};
        const auto field = [&](u32 offset, Id value, Id type, u32 size) {
            OpStore(BdaPointer(OpIAdd(U64, record, Constant(U64, static_cast<u64>(offset))), type),
                    value, spv::MemoryAccessMask::Aligned, size);
        };
        field(0, Constant(U64, SpvProgramIds() ? info.pgm_hash : u64{0}), U64, 8);
        field(8, Constant(U64, ~0ULL), U64, 8);
        field(16, guest_base, U64, 8);
        field(24, OpUConvert(U64, block_size), U64, 8);
        field(32, u64_zero_value, U64, 8);
        field(40, desc_bits, U64, 8);
        field(48, ConstU32(static_cast<u32>(hw_stage)), U32[1], 4);
        field(52, ConstU32(handle), U32[1], 4);
        field(56, index, U32[1], 4);
        field(60, ConstU32(i), U32[1], 4);
        field(64, ConstU32(elem_size), U32[1], 4);
        BdaEndIf(record_merge);
        BdaEndIf(nonzero_merge);
    }
    BdaEndIf(uncompared_merge);

    const Id merge = BdaBeginIf(valid);
    add_word(ShaderAddrVerifyCompared, ConstU32(1u));
    for (u32 i = 0; i < count; i++) {
        const Id elem_address = table ? table_addresses[i]
                                      : i == 0 ? address
                                               : OpIAdd(U64, address,
                                                        Constant(U64, static_cast<u64>(i * elem_size)));
        const Id address_value = OpLoad(elem_type, BdaPointer(elem_address, elem_type),
                                        spv::MemoryAccessMask::Aligned, elem_size);
        const Id address_bits{bits(address_value)};
        const Id desc_bits{bits(desc_values[i])};
        const Id differs{OpINotEqual(U1[1], address_bits, desc_bits)};
        const Id differ_merge = BdaBeginIf(differs);
        add_word(ShaderAddrVerifyMismatch, ConstU32(1u));
        const Id is_zero{OpIEqual(U1[1], address_bits, u64_zero_value)};
        add_word(ShaderAddrVerifyMismatchZero,
                 OpSelect(U32[1], is_zero, ConstU32(1u), u32_zero_value));
        // One record. The slot counter may pass the capacity: a slot past it is dropped.
        const Id slot{add_word(ShaderAddrVerifySlots, ConstU32(1u))};
        const Id fits{OpULessThan(U1[1], slot, ConstU32(ShaderAddrVerifyCapacity))};
        const Id record_merge = BdaBeginIf(fits);
        const Id record_offset{OpIMul(U64, OpUConvert(U64, slot),
                                      Constant(U64, static_cast<u64>(ShaderAddrVerifyRecordSize)))};
        const Id record{OpIAdd(U64, debug_base,
                               OpIAdd(U64, record_offset,
                                      Constant(U64, static_cast<u64>(ShaderAddrVerifyRecordsOffset))))};
        const auto field = [&](u32 offset, Id value, Id type, u32 size) {
            OpStore(BdaPointer(OpIAdd(U64, record, Constant(U64, static_cast<u64>(offset))), type),
                    value, spv::MemoryAccessMask::Aligned, size);
        };
        field(0, Constant(U64, SpvProgramIds() ? info.pgm_hash : u64{0}), U64, 8);
        field(8, elem_address, U64, 8);
        field(16, guest_base, U64, 8);
        field(24, OpUConvert(U64, block_size), U64, 8);
        field(32, address_bits, U64, 8);
        field(40, desc_bits, U64, 8);
        field(48, ConstU32(static_cast<u32>(hw_stage)), U32[1], 4);
        field(52, ConstU32(handle), U32[1], 4);
        field(56, index, U32[1], 4);
        field(60, ConstU32(i), U32[1], 4);
        field(64, ConstU32(elem_size), U32[1], 4);
        BdaEndIf(record_merge);
        BdaEndIf(differ_merge);
    }
    BdaEndIf(merge);

    return count == 1 ? desc_values[0] : OpCompositeConstruct(result_type, desc_values);
}

void EmitContext::HoistBdaBlocks() {
    // sotc: SOTC_GCN_TEX. The T# / S# probes of a heap stage run first: they declare the probe variables, which must
    // lead the entry block (emit_spirv_gcn_tex.cpp). No-op unless gcn_tex == 1 on a heap stage.
    HoistGcnTex();
    bda_blocks.resize(buffers.size());
    for (size_t handle = 0; handle < buffers.size(); ++handle) {
        if (!buffers[handle].bda || info.buffers[handle].used_types == IR::Type::Void) {
            continue;
        }
        bda_blocks[handle] = LoadBdaBlock(static_cast<u32>(handle));
    }
}

std::pair<Id, Id> EmitContext::BdaAddress(u32 handle, Id index, u32 elem_size, u32 count) {
    // sotc: SOTC_ADDR_TABLE: the address of element 0 through the table, and the validity of all `count` elements.
    if (handle < buffers.size() && buffers[handle].shader_addr && ShaderAddrTableMode(profile)) {
        boost::container::static_vector<Id, 4> addresses;
        const Id valid{TableAddresses(handle, index, elem_size, count, addresses)};
        return {addresses[0], valid};
    }
    // A hoisted block is valid in every block of the function; otherwise it is loaded here.
    const bool hoisted = handle < bda_blocks.size() && Sirit::ValidId(bda_blocks[handle].first);
    const auto [block_address, block_size] = hoisted ? bda_blocks[handle] : LoadBdaBlock(handle);
    const u32 elem_shift = static_cast<u32>(std::countr_zero(elem_size));
    const Id num_elements{OpShiftRightLogical(U32[1], block_size, ConstU32(elem_shift))};
    // The access fits when index <= num_elements - count (and count fits at all).
    const Id has_room{OpUGreaterThanEqual(U1[1], num_elements, ConstU32(count))};
    const Id last_index{OpISub(U32[1], num_elements, ConstU32(count))};
    const Id in_range{OpULessThanEqual(U1[1], index, last_index)};
    const Id valid{OpLogicalAnd(U1[1], has_room, in_range)};
    const Id safe_index{OpSelect(U32[1], valid, index, u32_zero_value)};
    const Id byte_offset{OpIMul(U64, OpUConvert(U64, safe_index),
                                Constant(U64, static_cast<u64>(elem_size)))};
    Id byte_address{OpIAdd(U64, block_address, byte_offset)};
    // sotc: SOTC_SHADER_ADDR (shader-addressed buffers only): the V# base need not be element aligned, but the
    // classic descriptor path binds at an aligned base and reads elements from it. So the address is aligned down
    // to elem_size: the same bytes as the descriptor, and the Aligned operand of the load or store holds.
    if (handle < buffers.size() && buffers[handle].shader_addr) {
        byte_address = OpBitwiseAnd(U64, byte_address,
                                    Constant(U64, ~static_cast<u64>(elem_size - 1)));
    }
    return {byte_address, valid};
}

Id EmitContext::BdaPointer(Id address, Id elem_type) {
    return OpConvertUToPtr(TypePointer(spv::StorageClass::PhysicalStorageBuffer, elem_type),
                           address);
}

Id EmitContext::BdaLoad(u32 handle, Id index, u32 elem_size, Id elem_type, Id zero,
                        Id result_type, u32 count) {
    if (handle < buffers.size() && buffers[handle].shader_addr && ShaderAddrTableMode(profile)) {
        return BdaLoadTable(handle, index, elem_size, elem_type, zero, result_type, count); // sotc: SOTC_ADDR_TABLE
    }
    const auto [address, valid] = BdaAddress(handle, index, elem_size, count);
    // sotc: SOTC_BDA: the loads run only when the access is valid, as the guarded atomics do
    // (emit_spirv_atomic.cpp). BdaAddress picks the block start for an invalid access, and a
    // physical-address load there is not bounded by robustBufferAccess (a block smaller than the
    // access reads past its end). The phi takes zero from the header.
    const Id header = last_label;
    const Id merge = BdaBeginIf(valid);
    boost::container::static_vector<Id, 4> values;
    for (u32 i = 0; i < count; i++) {
        const Id elem_address = i == 0 ? address
                                       : OpIAdd(U64, address,
                                                Constant(U64, static_cast<u64>(i * elem_size)));
        values.push_back(OpLoad(elem_type, BdaPointer(elem_address, elem_type),
                                spv::MemoryAccessMask::Aligned, elem_size));
    }
    const Id then_end = last_label;
    BdaEndIf(merge);
    boost::container::static_vector<Id, 4> ids;
    for (u32 i = 0; i < count; i++) {
        ids.push_back(OpPhi(elem_type, values[i], then_end, zero, header));
    }
    return count == 1 ? ids[0] : OpCompositeConstruct(result_type, ids);
}

void EmitContext::BdaStore(u32 handle, Id index, u32 elem_size, Id elem_type, Id value,
                           u32 count) {
    if (handle < buffers.size() && buffers[handle].shader_addr && ShaderAddrTableMode(profile)) {
        BdaStoreTable(handle, index, elem_size, elem_type, value, count); // sotc: SOTC_ADDR_TABLE
        return;
    }
    const auto [address, valid] = BdaAddress(handle, index, elem_size, count);
    const Id merge{BdaBeginIf(valid)};
    for (u32 i = 0; i < count; i++) {
        const Id elem_address = i == 0 ? address
                                       : OpIAdd(U64, address,
                                                Constant(U64, static_cast<u64>(i * elem_size)));
        const Id elem_value = count == 1 ? value : OpCompositeExtract(elem_type, value, i);
        OpStore(BdaPointer(elem_address, elem_type), elem_value, spv::MemoryAccessMask::Aligned,
                elem_size);
    }
    BdaEndIf(merge);
}

// sotc: SOTC_ADDR_TABLE (table modes). The device addresses of the `count` elements of a shader-addressed buffer, one
// table lookup per element. Element i sits at guest address g = (base + (index + i) * elem_size) & ~(elem_size - 1), the
// same alignment as BdaAddress. Its page is p = (g - window) >> 14 and the table entry at table + 8 * p is the device
// address of that page's first byte (0 when not resident). The address is entry + (g & 0x3fff). Returns valid: the
// block holds the access AND every element has a non-zero entry. The entry loads are emitted before the caller's
// branch, so they must be safe on an invalid access: then the page is taken as 0 (table[0] is a valid read).
//
// Example, a 2-element load (count 2, elem 4) that straddles a page boundary: element 0 at guest 0x700003FFC (page 0,
// entry E0) and element 1 at guest 0x700004000 (page 1, entry E1). Both pages are resident, so the access is valid,
// and the elements are at E0 + 0x3FFC and E1 + 0x0. They need not be contiguous (two backings), which is why each
// element has its own entry and the loads never add i * elem_size to the first address.
Id EmitContext::TableAddresses(u32 handle, Id index, u32 elem_size, u32 count,
                               boost::container::static_vector<Id, 4>& addresses,
                               boost::container::static_vector<Id, 4>* elem_valid) {
    const bool hoisted = handle < bda_blocks.size() && Sirit::ValidId(bda_blocks[handle].first);
    const auto [guest_base, block_size] = hoisted ? bda_blocks[handle] : LoadBdaBlock(handle);
    const u32 elem_shift = static_cast<u32>(std::countr_zero(elem_size));
    const Id num_elements{OpShiftRightLogical(U32[1], block_size, ConstU32(elem_shift))};
    const Id has_room{OpUGreaterThanEqual(U1[1], num_elements, ConstU32(count))};
    const Id last_index{OpISub(U32[1], num_elements, ConstU32(count))};
    const Id in_range{OpULessThanEqual(U1[1], index, last_index)};
    const Id valid{OpLogicalAnd(U1[1], has_room, in_range)};
    const Id safe_index{OpSelect(U32[1], valid, index, u32_zero_value)};

    // The table words of the packet (mirror_guest, mirror_device): window base and the table's device address.
    const auto push_word = [&](u32 member) {
        return OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64),
                                         push_data_block, ConstU32(shader_addr_member + member)));
    };
    const Id window_guest{push_word(0)};
    const Id table_device{push_word(1)};

    addresses.clear();
    if (elem_valid) {
        elem_valid->clear();
    }
    // sotc: SOTC_GCN_BUF (driver compile cost, first-run measurements of 4 Nov 2026). The elements of one access span
    // at most 4 * 8 bytes, so they lie on at most two pages: the page of the first element and the page of the last
    // one. Two table lookups per access (one for a single element) and one page-miss site, instead of one lookup and
    // one inline miss record per element: the inline records made a cold compute pipeline 2.6 times slower to build.
    //
    // Per element (elem_valid, loads): element i is in the block when index + i < num_elements. The first element
    // decides whether the access touches the block at all; the last lookup is at the last element that is still in
    // the block (min(index + count - 1, num_elements - 1)), so every valid element is on one of the two pages. The
    // generic formatted load (lower_buffer_format_to_raw.cpp) always reads 4 dwords, so the last record of a buffer
    // reads past its end: judged as one access, every component of that record read zero (broken vertices, s2 runs).
    const Id first_in{elem_valid ? OpULessThan(U1[1], index, num_elements) : valid};
    const Id first_index{elem_valid ? OpSelect(U32[1], first_in, index, u32_zero_value) : safe_index};
    Id last_index_used{first_index};
    if (count > 1) {
        const Id wanted{OpIAdd(U32[1], first_index, ConstU32(count - 1))};
        if (elem_valid) {
            const Id last_in_block{OpISub(U32[1], OpSelect(U32[1], first_in, num_elements, u32_one_value),
                                          u32_one_value)};
            last_index_used = OpSelect(U32[1], OpULessThan(U1[1], wanted, last_in_block), wanted, last_in_block);
        } else {
            last_index_used = wanted;
        }
    }
    const auto guest_of = [&](Id elem_index) {
        return OpBitwiseAnd(
            U64,
            OpIAdd(U64, guest_base,
                   OpIMul(U64, OpUConvert(U64, elem_index), Constant(U64, static_cast<u64>(elem_size)))),
            Constant(U64, ~static_cast<u64>(elem_size - 1)));
    };
    // An access outside the block uses page 0 (its entry load stays inside the table).
    const auto entry_of = [&](Id guest) {
        const Id rel{OpSelect(U64, first_in, OpISub(U64, guest, window_guest), u64_zero_value)};
        const Id page{OpShiftRightLogical(U64, rel, Constant(U64, 14ULL))};
        const Id entry_address{OpIAdd(U64, table_device, OpIMul(U64, page, Constant(U64, 8ULL)))};
        return OpLoad(U64, BdaPointer(entry_address, U64), spv::MemoryAccessMask::Aligned, 8u);
    };
    const Id page_mask{Constant(U64, ~((1ULL << 14) - 1))};
    const Id guest_first{guest_of(first_index)};
    const Id entry_first{entry_of(guest_first)};
    const Id guest_last{count > 1 ? guest_of(last_index_used) : guest_first};
    const Id entry_last{count > 1 ? entry_of(guest_last) : entry_first};
    const Id page_first{OpBitwiseAnd(U64, guest_first, page_mask)};

    // sotc: SOTC_GCN_BUF. An access in the block with a page that is not resident appends one page miss (the first
    // missing page; gcn_layout.h). Only the lanes with a zero entry run the branch; the record code is one function.
    if (profile.gcn_buf != 0 && GcnMissEmit()) { // sotc: only with SOTC_GCN_MISS_EMIT=1 (default off, info.h)
        const Id first_missing{OpIEqual(U1[1], entry_first, u64_zero_value)};
        const Id any_missing{count > 1 ? OpLogicalOr(U1[1], first_missing,
                                                     OpIEqual(U1[1], entry_last, u64_zero_value))
                                       : first_missing};
        const Id miss{OpLogicalAnd(U1[1], first_in, any_missing)};
        const Id miss_page{count > 1 ? OpSelect(U64, first_missing, page_first,
                                                OpBitwiseAnd(U64, guest_last, page_mask))
                                     : page_first};
        const Id miss_merge{BdaBeginIf(miss)};
        const Id miss_flags{GcnPageMissFlags(handle)}; // sotc: formatted / written, and the V#, in the record
        if (Sirit::ValidId(gcn_page_miss_func)) {
            OpFunctionCall(void_id, gcn_page_miss_func, miss_page, miss_flags, guest_base, block_size);
        } else {
            GcnPageMiss(miss_page, miss_flags, guest_base, block_size);
        }
        BdaEndIf(miss_merge);
    }

    Id all_present{};
    for (u32 i = 0; i < count; i++) {
        Id elem_in{};
        Id guest{};
        if (elem_valid) {
            elem_in = OpLogicalAnd(U1[1], OpUGreaterThan(U1[1], num_elements, ConstU32(i)),
                                   OpULessThanEqual(U1[1], index,
                                                    OpISub(U32[1], num_elements, ConstU32(i + 1))));
            guest = i == 0 ? guest_first : guest_of(OpSelect(U32[1], elem_in, OpIAdd(U32[1], index, ConstU32(i)),
                                                             first_index));
        } else {
            guest = i == 0 ? guest_first : guest_of(OpIAdd(U32[1], first_index, ConstU32(i)));
        }
        const Id entry{i == 0 ? entry_first
                              : OpSelect(U64, OpIEqual(U1[1], OpBitwiseAnd(U64, guest, page_mask), page_first),
                                         entry_first, entry_last)};
        const Id elem_address{OpIAdd(U64, entry, OpBitwiseAnd(U64, guest, Constant(U64, (1ULL << 14) - 1)))};
        const Id present{OpINotEqual(U1[1], entry, u64_zero_value)};
        if (elem_valid) {
            const Id ok{OpLogicalAnd(U1[1], elem_in, present)};
            elem_valid->push_back(ok);
            addresses.push_back(OpSelect(U64, ok, elem_address, table_device)); // the table: 8-byte aligned, readable
        } else {
            addresses.push_back(elem_address);
        }
        all_present = i == 0 ? present : OpLogicalAnd(U1[1], all_present, present);
    }
    return OpLogicalAnd(U1[1], valid, all_present);
}

// sotc: SOTC_GCN shared helpers. murmur3_32 over u32 words with a seed: the operations of Shader::Gcn::Hash
// (gcn_layout.h), in the same order, so a key hashed here and on the host lands in the same slot.
Id EmitContext::GcnHash(std::span<const Id> words, u32 seed) {
    const auto rotl = [&](Id x, u32 r) {
        return OpBitwiseOr(U32[1], OpShiftLeftLogical(U32[1], x, ConstU32(r)),
                           OpShiftRightLogical(U32[1], x, ConstU32(32u - r)));
    };
    Id h{ConstU32(seed ^ 0x9E3779B9u)};
    for (const Id word : words) {
        Id k{OpIMul(U32[1], word, ConstU32(0xCC9E2D51u))};
        k = OpIMul(U32[1], rotl(k, 15), ConstU32(0x1B873593u));
        h = OpBitwiseXor(U32[1], h, k);
        h = OpIAdd(U32[1], OpIMul(U32[1], rotl(h, 13), ConstU32(5u)), ConstU32(0xE6546B64u));
    }
    h = OpBitwiseXor(U32[1], h, ConstU32(static_cast<u32>(words.size()) * 4u));
    h = OpBitwiseXor(U32[1], h, OpShiftRightLogical(U32[1], h, ConstU32(16u)));
    h = OpIMul(U32[1], h, ConstU32(0x85EBCA6Bu));
    h = OpBitwiseXor(U32[1], h, OpShiftRightLogical(U32[1], h, ConstU32(13u)));
    h = OpIMul(U32[1], h, ConstU32(0xC2B2AE35u));
    h = OpBitwiseXor(U32[1], h, OpShiftRightLogical(U32[1], h, ConstU32(16u)));
    return h;
}

// sotc: SOTC_GCN_BUF. Records a page miss for the guest page `page` (U64, page aligned). The globals buffer is the
// push word of ShaderAddrArenaGuestOffset (member 2 of the window words; the host sends it for gcn pipelines). The
// seen bit of the page is set with an atomic OR; a clear bit means the page is new since the last drain, so one
// record is appended: index from atomicIAdd on write_index, body first, stamp (index + 1) last (gcn_layout.h).
// Called inside a branch that runs only for a missing page; the record is dropped when the ring is full (the slot
// wraps, the host counts the overflow).
// sotc: the flags of a page miss of buffer `handle`: what the host bind passes to Touch (is_formatted, is_written).
Id EmitContext::GcnPageMissFlags(u32 handle) {
    u32 flags = 0;
    if (handle < info.buffers.size()) {
        const auto& desc = info.buffers[handle];
        flags = (desc.is_formatted ? Shader::Gcn::kMissPageFormatted : 0u) |
                (desc.is_written ? Shader::Gcn::kMissPageWritten : 0u);
    }
    return ConstU32(flags);
}

void EmitContext::GcnPageMiss(Id page, Id flags, Id guest_base, Id block_size) {
    const Id globals{OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64),
                                               push_data_block, ConstU32(shader_addr_member + 2)))};
    const auto field = [&](u32 offset) { // a u32 field of the globals buffer
        return BdaPointer(OpIAdd(U64, globals, Constant(U64, static_cast<u64>(offset))), U32[1]);
    };
    const Id scope{ConstU32(static_cast<u32>(spv::Scope::Device))};

    const Id lo{OpUConvert(U32[1], page)};
    const Id hi{OpUConvert(U32[1], OpShiftRightLogical(U64, page, Constant(U64, 32ULL)))};
    const std::array words{lo, hi};
    const Id hash{GcnHash(words, static_cast<u32>(Shader::Gcn::kMissPage))};
    const Id bit{OpBitwiseAnd(U32[1], hash, ConstU32(Shader::Gcn::kSeenBits - 1))};
    const Id seen_offset{OpIAdd(U32[1], OpShiftLeftLogical(U32[1], OpShiftRightLogical(U32[1], bit, ConstU32(5u)),
                                                           ConstU32(2u)),
                                ConstU32(Shader::Gcn::kSeenOffset))};
    const Id seen_word{BdaPointer(OpIAdd(U64, globals, OpUConvert(U64, seen_offset)), U32[1])};
    const Id mask{OpShiftLeftLogical(U32[1], ConstU32(1u), OpBitwiseAnd(U32[1], bit, ConstU32(31u)))};
    const Id old{OpAtomicOr(U32[1], seen_word, scope, u32_zero_value, mask)};
    const Id fresh{OpIEqual(U1[1], OpBitwiseAnd(U32[1], old, mask), u32_zero_value)};

    const Id fresh_merge{BdaBeginIf(fresh)};
    const Id idx{OpAtomicIAdd(U32[1], field(Shader::Gcn::kMissRingOffset), scope, u32_zero_value,
                              ConstU32(1u))};
    const Id slot{OpBitwiseAnd(U32[1], idx, ConstU32(Shader::Gcn::kMissRingCapacity - 1))};
    // Record base: the ring header is 16 bytes, then MissRecord (48 bytes) per slot.
    const Id record_offset{OpIAdd(U32[1], ConstU32(Shader::Gcn::kMissRingOffset + 16),
                                  OpIMul(U32[1], slot, ConstU32(static_cast<u32>(sizeof(Shader::Gcn::MissRecord)))))};
    const Id record{OpIAdd(U64, globals, OpUConvert(U64, record_offset))};
    const auto body = [&](u32 offset, Id value) {
        OpStore(BdaPointer(OpIAdd(U64, record, Constant(U64, static_cast<u64>(offset))), U32[1]), value,
                spv::MemoryAccessMask::Aligned, 4u);
    };
    body(4, ConstU32(static_cast<u32>(Shader::Gcn::kMissPage))); // kind
    body(8, flags);                                              // view_class: the page miss flags (sotc)
    body(12, u32_zero_value);                                    // reserved
    body(16, lo);                                                // words[0]
    body(20, hi);                                                // words[1]
    // sotc: words[2..4]: the V# guest base and block size (gcn_layout.h, kMissPage payload).
    body(24, OpUConvert(U32[1], guest_base));
    body(28, OpUConvert(U32[1], OpShiftRightLogical(U64, guest_base, Constant(U64, 32ULL))));
    body(32, block_size);
    // sotc: the stamp, last, as an atomic store with Release semantics: the body stores above are made available
    // before it (a plain store gave no order, so the host could see the stamp with a stale body).
    GcnStoreStamp(record, OpIAdd(U32[1], idx, ConstU32(1u)));
    BdaEndIf(fresh_merge);
}

// sotc: SOTC_GCN_BUF / SOTC_GCN_TEX. The stamp of a miss record (U64 address of the record's first word): an
// OpAtomicStore, Device scope, Release | UniformMemory (UniformMemory covers PhysicalStorageBuffer in Vulkan).
void EmitContext::GcnStoreStamp(Id record_address, Id stamp) {
    const Id scope{ConstU32(static_cast<u32>(spv::Scope::Device))};
    const Id semantics{ConstU32(static_cast<u32>(spv::MemorySemanticsMask::Release |
                                                 spv::MemorySemanticsMask::UniformMemory))};
    OpAtomicStore(BdaPointer(record_address, U32[1]), scope, semantics, stamp);
}

// sotc: SOTC_ADDR_TABLE. BdaLoad with one translated address per element (TableAddresses). Each element is valid on
// its own, as with a descriptor (robust access per element): an element past the block end or on a page that is not
// resident reads zero, the others read their bytes. An invalid element's address is the table itself (always
// readable), so the loads need no branch.
Id EmitContext::BdaLoadTable(u32 handle, Id index, u32 elem_size, Id elem_type, Id zero,
                             Id result_type, u32 count) {
    boost::container::static_vector<Id, 4> ids;
    if (Sirit::ValidId(gcn_table_addr_func)) { // sotc: SOTC_SPV_FUNCS: one call per access (gcn_table_addr)
        const bool hoisted = handle < bda_blocks.size() && Sirit::ValidId(bda_blocks[handle].first);
        const auto [guest_base, block_size] = hoisted ? bda_blocks[handle] : LoadBdaBlock(handle);
        const u32 elem_shift = static_cast<u32>(std::countr_zero(elem_size));
        const Id packed{OpFunctionCall(gcn_table_addr_result, gcn_table_addr_func, guest_base, block_size, index,
                                       ConstU32(elem_shift), ConstU32(count),
                                       GcnPageMissFlags(handle))}; // sotc: the miss flags (gcn_table_addr)
        const Id addresses{OpCompositeExtract(gcn_table_u64x4, packed, 0u)};
        const Id oks{OpCompositeExtract(U32[4], packed, 1u)};
        for (u32 i = 0; i < count; i++) {
            const Id address{OpCompositeExtract(U64, addresses, i)};
            const Id ok{OpINotEqual(U1[1], OpCompositeExtract(U32[1], oks, i), u32_zero_value)};
            const Id value{OpLoad(elem_type, BdaPointer(address, elem_type), spv::MemoryAccessMask::Aligned,
                                  elem_size)};
            ids.push_back(OpSelect(elem_type, ok, value, zero));
        }
        return count == 1 ? ids[0] : OpCompositeConstruct(result_type, ids);
    }
    boost::container::static_vector<Id, 4> addresses;
    boost::container::static_vector<Id, 4> elem_valid;
    TableAddresses(handle, index, elem_size, count, addresses, &elem_valid);
    for (u32 i = 0; i < count; i++) {
        const Id value{OpLoad(elem_type, BdaPointer(addresses[i], elem_type), spv::MemoryAccessMask::Aligned,
                              elem_size)};
        ids.push_back(OpSelect(elem_type, elem_valid[i], value, zero));
    }
    return count == 1 ? ids[0] : OpCompositeConstruct(result_type, ids);
}

// sotc: SOTC_ADDR_TABLE. BdaStore with one translated address per element. The branch covers every element, so a
// store with any non-resident page is skipped whole (the access is not split).
void EmitContext::BdaStoreTable(u32 handle, Id index, u32 elem_size, Id elem_type, Id value,
                                u32 count) {
    boost::container::static_vector<Id, 4> addresses;
    const Id valid{TableAddresses(handle, index, elem_size, count, addresses)};
    const Id merge{BdaBeginIf(valid)};
    for (u32 i = 0; i < count; i++) {
        const Id elem_value = count == 1 ? value : OpCompositeExtract(elem_type, value, i);
        OpStore(BdaPointer(addresses[i], elem_type), elem_value, spv::MemoryAccessMask::Aligned,
                elem_size);
    }
    BdaEndIf(merge);
}

Id EmitContext::BdaBeginIf(Id valid) {
    const Id then_label{OpLabel()};
    const Id merge{OpLabel()};
    OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    OpBranchConditional(valid, then_label, merge);
    AddLabel(then_label);
    return merge;
}

void EmitContext::BdaEndIf(Id merge) {
    OpBranch(merge);
    AddLabel(merge);
}

void EmitContext::DefineBuffers() {
    for (const auto& desc : info.buffers) {
        const auto buf_sharp = desc.GetSharp(info);
        // MTYPE class 3 marks guest buffers shared between invocations; shared
        // memory lowered to a storage buffer needs the same guarantee.
        // sotc: SOTC_COMPUTE_SPECFREE: the V# of the translation is not the dispatch's (MTYPE is not keyed), so every
        // written guest buffer of a compute stage is coherent.
        const bool is_coherent =
            desc.buffer_type == BufferType::SharedMemory || buf_sharp.mtype == 3 ||
            (SpecFreeStage(info) && desc.is_written && desc.buffer_type == BufferType::Guest);

        // Set indexes for special buffers.
        if (desc.buffer_type == BufferType::Flatbuf) {
            flatbuf_index = buffers.size();
        } else if (desc.buffer_type == BufferType::BdaPagetable) {
            bda_pagetable_index = buffers.size();
        } else if (desc.buffer_type == BufferType::FaultBuffer) {
            fault_buffer_index = buffers.size();
        } else if (desc.buffer_type == BufferType::LodFeedback) {
            lod_feedback_index = buffers.size();
        } else if (desc.buffer_type == BufferType::BdaBlockTable) {
            bda_table_index = buffers.size();
        }

        // Define aliases depending on the shader usage.
        auto& spv_buffer = buffers.emplace_back(binding.buffer++, desc.buffer_type);
        // sotc: SOTC_SHADER_ADDR: the block comes from the V# words (LoadShaderAddrBlock); no descriptor.
        if (ShaderAddrBuffer(profile, info, desc)) {
            spv_buffer.shader_addr = true;
            if (!ShaderAddrVerifyMode(profile)) {
                spv_buffer.bda = true;
                ++binding.unified;
                continue;
            }
            // sotc: SOTC_SHADER_ADDR=2 (verify): the classic descriptor aliases below, as in mode 0. bda stays off,
            // so stores and atomics use the descriptor; the loads compare (ShaderAddrVerifyLoad).
            spv_buffer.shader_addr_verify = true;
        }
        // sotc: SOTC_BDA: a guest buffer that is not texel or coherent takes no descriptor; its
        // block table slot gives its address. Coherent buffers keep the descriptor (Coherent
        // decoration, which physical pointers do not get).
        if (!spv_buffer.shader_addr_verify && profile.bda_buffers && info.HasBdaBlockTable() &&
            desc.buffer_type == BufferType::Guest && !desc.is_formatted && !is_coherent) {
            spv_buffer.bda = true;
            ++binding.unified;
            continue;
        }
        if (True(desc.used_types & IR::Type::U64)) {
            spv_buffer.Alias(PointerType::U64) =
                DefineBuffer(desc.is_written, is_coherent, 3, desc.buffer_type, U64);
        }
        if (True(desc.used_types & IR::Type::U32)) {
            spv_buffer.Alias(PointerType::U32) =
                DefineBuffer(desc.is_written, is_coherent, 2, desc.buffer_type, U32[1]);
        }
        if (True(desc.used_types & IR::Type::F32)) {
            spv_buffer.Alias(PointerType::F32) =
                DefineBuffer(desc.is_written, is_coherent, 2, desc.buffer_type, F32[1]);
        }
        if (True(desc.used_types & IR::Type::U16)) {
            spv_buffer.Alias(PointerType::U16) =
                DefineBuffer(desc.is_written, is_coherent, 1, desc.buffer_type, U16);
        }
        if (True(desc.used_types & IR::Type::U8)) {
            spv_buffer.Alias(PointerType::U8) =
                DefineBuffer(desc.is_written, is_coherent, 0, desc.buffer_type, U8);
        }
        ++binding.unified;
    }
}

spv::ImageFormat GetFormat(const AmdGpu::Image& image) {
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format32 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Uint) {
        return spv::ImageFormat::R32ui;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format32 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Sint) {
        return spv::ImageFormat::R32i;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format32 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Float) {
        return spv::ImageFormat::R32f;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format32_32 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Float) {
        return spv::ImageFormat::Rg32f;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format32_32 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Uint) {
        return spv::ImageFormat::Rg32ui;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format32_32_32_32 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Uint) {
        return spv::ImageFormat::Rgba32ui;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format16 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Float) {
        return spv::ImageFormat::R16f;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format16 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Uint) {
        return spv::ImageFormat::R16ui;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format16_16 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Float) {
        return spv::ImageFormat::Rg16f;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format16_16 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Snorm) {
        return spv::ImageFormat::Rg16Snorm;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format8_8 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Unorm) {
        return spv::ImageFormat::Rg8;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format16_16_16_16 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Float) {
        return spv::ImageFormat::Rgba16f;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format16_16_16_16 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Unorm) {
        return spv::ImageFormat::Rgba16;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format8 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Unorm) {
        return spv::ImageFormat::R8;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format8_8_8_8 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Unorm) {
        return spv::ImageFormat::Rgba8;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format8_8_8_8 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Uint) {
        return spv::ImageFormat::Rgba8ui;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format10_11_11 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Float) {
        return spv::ImageFormat::R11fG11fB10f;
    }
    if (image.GetDataFmt() == AmdGpu::DataFormat::Format32_32_32_32 &&
        image.GetNumberFmt() == AmdGpu::NumberFormat::Float) {
        return spv::ImageFormat::Rgba32f;
    }
    UNREACHABLE_MSG("Unknown storage format data_format={}, num_format={}", image.GetDataFmt(),
                    image.GetNumberFmt());
}

Id ImageType(EmitContext& ctx, const ImageResource& desc, Id sampled_type) {
    const auto image = desc.GetSharp(ctx.info);
    const auto format = desc.is_atomic ? GetFormat(image) : spv::ImageFormat::Unknown;
    const auto type = image.GetViewType(desc.is_array);
    const u32 sampled = desc.is_written ? 2 : 1;
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color2D:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim2D, false, false, false, sampled, format);
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim2D, false, true, false, sampled, format);
    case AmdGpu::ImageType::Color2DMsaa:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim2D, false, false, true, sampled, format);
    case AmdGpu::ImageType::Color3D:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim3D, false, false, false, sampled, format);
    default:
        break;
    }
    UNREACHABLE_MSG("Invalid texture type {}", type);
}

bool UsesHeapImages(const Info& info, const Profile& profile) {
    // sotc: SOTC_HEAP. The table load needs 64-bit integers, and the profile switches the heap.
    if (!profile.heap_images || !profile.support_int64) {
        return false;
    }
    // sotc: the stage rule (Shader::HeapStageRule), cached once the Info is complete.
    if (info.bindless.ready) {
        return info.bindless.heap_stage;
    }
    return Shader::HeapStageRule(info);
}

void EmitContext::DefineImagesAndSamplers() {
    uses_heap_images = UsesHeapImages(info, profile);
    for (u32 image_index = 0; image_index < info.images.size(); ++image_index) {
        const auto& image_desc = info.images[image_index];
        const auto sharp = image_desc.GetSharp(info);
        const auto nfmt = sharp.GetNumberFmt();
        const bool is_integer = AmdGpu::IsInteger(nfmt);
        const bool is_storage = image_desc.is_written;
        const MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const VectorIds& data_types = GetAttributeType(*this, nfmt);
        const Id sampled_type = data_types[1];

        // sotc: SOTC_N5_BINDLESS: a sampled image of the heap has no variable. Its texture entry
        // loads it from the runtime array of its key (LoadImage) at the index of its push slot.
        // Every resource of a key shares the key's image type, so the array elements match.
        const u32 bindless_slot = BindlessImageSlotOf(info, image_index);
        if (bindless_slot != NO_BINDLESS_IMAGE_SLOT) {
            const u32 key = BindlessImageKeyOf(info, image_desc);
            if (!Sirit::ValidId(bindless_image_types[key])) {
                bindless_image_types[key] = ImageType(*this, image_desc, sampled_type);
                const Id array_type{TypeRuntimeArray(bindless_image_types[key])};
                const Id pointer_type{TypePointer(spv::StorageClass::UniformConstant, array_type)};
                bindless_image_arrays[key] =
                    AddGlobalVariable(pointer_type, spv::StorageClass::UniformConstant);
                Decorate(bindless_image_arrays[key], spv::Decoration::Binding, 1U + key);
                Decorate(bindless_image_arrays[key], spv::Decoration::DescriptorSet, 1U);
                Name(bindless_image_arrays[key],
                     fmt::format("{}_bindless_img{}", hw_stage, key));
                interfaces.push_back(bindless_image_arrays[key]);
            }
            uses_bindless_images = true;
            // sotc: a storage image keeps its storage type (sampled = 2, see ImageType) and is read
            // and written through LoadImage, like the sampled ones.
            images.push_back({
                .data_types = &data_types,
                .id = Id{},
                .sampled_type = is_storage ? sampled_type
                                           : TypeSampledImage(bindless_image_types[key]),
                .image_type = bindless_image_types[key],
                .view_type = sharp.GetViewType(image_desc.is_array),
                .is_integer = is_integer,
                .is_storage = is_storage,
                .mip_fallback_mode = mip_fallback_mode,
                .array_size = 0,
                .bindless = true,
                .bindless_slot = bindless_slot,
                .bindless_key = key,
                .heap = uses_heap_images,
                .table_entry = image_index,
            });
            continue;
        }

        const Id image_type{ImageType(*this, image_desc, sampled_type)};

        const u32 num_bindings = image_desc.NumBindings(info);
        Id pointee_type = image_type;
        if (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex ||
            image_desc.array_size != 0) {
            pointee_type = TypeArray(pointee_type, ConstU32(num_bindings));
        }

        const Id pointer_type{TypePointer(spv::StorageClass::UniformConstant, pointee_type)};
        const Id id{AddGlobalVariable(pointer_type, spv::StorageClass::UniformConstant)};
        Decorate(id, spv::Decoration::Binding, binding.unified);
        binding.unified += num_bindings;
        Decorate(id, spv::Decoration::DescriptorSet, DescriptorSetIndex());
        Name(id, fmt::format("{}_{}{}", hw_stage, "img", images.size()));
        images.push_back({
            .data_types = &data_types,
            .id = id,
            .sampled_type = is_storage ? sampled_type : TypeSampledImage(image_type),
            .image_type = image_type,
            .view_type = sharp.GetViewType(image_desc.is_array),
            .is_integer = is_integer,
            .is_storage = is_storage,
            .mip_fallback_mode = mip_fallback_mode,
            .array_size = image_desc.array_size,
        });
        interfaces.push_back(id);
    }
    if (std::ranges::any_of(info.images,
                            [](const ImageResource& image) { return image.is_atomic; })) {
        image_u32 = TypePointer(spv::StorageClass::Image, U32[1]);
        image_f32 = TypePointer(spv::StorageClass::Image, F32[1]);
    }
    if (info.samplers.empty()) {
        return;
    }
    sampler_type = TypeSampler();
    sampler_pointer_type = TypePointer(spv::StorageClass::UniformConstant, sampler_type);
    if (UsesBindlessSamplers(info)) {
        // sotc: SOTC_BINDLESS_SAMP: one runtime array of the global sampler heap (set 1). The
        // samplers of this stage are indexed through the push constants, see LoadSampler.
        uses_bindless_samplers = true;
        bindless_sampler_base = GetBindlessSamplerRange(info.sw_stage).base;
        const Id array_type{TypeRuntimeArray(sampler_type)};
        const Id pointer_type{TypePointer(spv::StorageClass::UniformConstant, array_type)};
        bindless_sampler_array = AddGlobalVariable(pointer_type, spv::StorageClass::UniformConstant);
        Decorate(bindless_sampler_array, spv::Decoration::Binding, 0U);
        Decorate(bindless_sampler_array, spv::Decoration::DescriptorSet, 1U);
        Name(bindless_sampler_array, fmt::format("{}_bindless_samp", hw_stage));
        interfaces.push_back(bindless_sampler_array);
        return;
    }
    for (const auto& samp_desc : info.samplers) {
        const Id id{AddGlobalVariable(sampler_pointer_type, spv::StorageClass::UniformConstant)};
        Decorate(id, spv::Decoration::Binding, binding.unified++);
        Decorate(id, spv::Decoration::DescriptorSet, DescriptorSetIndex());
        Name(id, fmt::format("{}_{}{}", hw_stage, "samp", samplers.size()));
        samplers.push_back(id);
        interfaces.push_back(id);
    }
}

u32 HeapMemberShift(const Profile& profile) {
    // sotc: SOTC_GCN_TEX. The globals member exists only with gcn_tex on a heap stage (see HeapGcnGlobalsMember).
    return profile.gcn_tex == 1 ? 1u : 0u;
}

u32 HeapTableMember(const Info& info, const Profile& profile) {
    // sotc: SOTC_HEAP_VS. A vertex heap stage reads its table address at byte 136, every other heap stage at byte 64.
    return info.sw_stage == Shader::SwStage::Vertex ? HeapVsTableMember + HeapMemberShift(profile)
                                                    : HeapIndexTableMember;
}

Id EmitContext::LoadTableIndex(u32 entry) {
    // sotc: SOTC_HEAP: the table address is the u64 push member of the stage (HeapTableMember: byte 64, or 136 for a
    // vertex heap stage); entry i is one u32 at address + 4 * i. The index is the same in every invocation of a draw:
    // it comes from push data and a constant entry, so it is dynamically uniform and needs no NonUniform decoration.
    const Id ptr{OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64), push_data_block,
                               ConstU32(HeapTableMember(info, profile)))};
    const Id table{OpLoad(U64, ptr)};
    const Id address{OpIAdd(U64, table, Constant(U64, static_cast<u64>(entry) * sizeof(u32)))};
    return OpLoad(U32[1], BdaPointer(address, U32[1]), spv::MemoryAccessMask::Aligned, 4U);
}

Id EmitContext::HeapDebugBase() {
    return OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64), push_data_block,
                                     ConstU32(HeapDebugMember + HeapMemberShift(profile))));
}

Id EmitContext::HeapAdd(Id debug_base, u32 offset, Id value) {
    const Id scope{ConstU32(static_cast<u32>(spv::Scope::Device))};
    const Id ptr{BdaPointer(OpIAdd(U64, debug_base, Constant(U64, static_cast<u64>(offset))), U32[1])};
    return OpAtomicIAdd(U32[1], ptr, scope, u32_zero_value, value);
}

Id EmitContext::LoadHeapClassic(u32 member, u32 shift) {
    // The compact classic words (resource.h): a 16-bit slot in the low (shift 0) or high (shift 16) half.
    const Id ptr{OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U32[1]), push_data_block,
                               ConstU32(member))};
    return OpBitFieldUExtract(U32[1], OpLoad(U32[1], ptr), ConstU32(shift), ConstU32(16U));
}

void EmitContext::HeapRecord(Id bad, u32 kind, u32 number, Id classic, Id raw, u32 entry, u32 count_word) {
    // sotc: SOTC_HEAP. A branch on `bad` only: the record is written by one invocation's branch, and the slot
    // counter may pass the capacity (a slot past it is dropped, and counted in HeapDebugSlots).
    const Id merge{BdaBeginIf(bad)};
    const Id debug_base{HeapDebugBase()};
    HeapAdd(debug_base, count_word, ConstU32(1u));
    const Id slot{HeapAdd(debug_base, HeapDebugSlots, ConstU32(1u))};
    const Id fits{OpULessThan(U1[1], slot, ConstU32(Shader::HeapRecordCapacity))};
    const Id record_merge{BdaBeginIf(fits)};
    const Id record_offset{OpIMul(U64, OpUConvert(U64, slot), Constant(U64, u64{Shader::HeapRecordSize}))};
    const Id record{OpIAdd(U64, debug_base,
                           OpIAdd(U64, record_offset, Constant(U64, u64{Shader::HeapRecordsOffset})))};
    const auto field = [&](u32 offset, Id value, Id type, u32 size) {
        OpStore(BdaPointer(OpIAdd(U64, record, Constant(U64, static_cast<u64>(offset))), type), value,
                spv::MemoryAccessMask::Aligned, size);
    };
    field(0, Constant(U64, SpvProgramIds() ? info.pgm_hash : u64{0}), U64, 8);
    field(8,
          OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64), push_data_block,
                                    ConstU32(HeapTableMember(info, profile)))),
          U64, 8);
    field(16, ConstU32(static_cast<u32>(hw_stage)), U32[1], 4);
    field(20, ConstU32(kind), U32[1], 4);
    field(24, ConstU32(number), U32[1], 4);
    field(28, classic, U32[1], 4);
    field(32, raw, U32[1], 4);
    field(36, ConstU32(entry), U32[1], 4);
    BdaEndIf(record_merge);
    BdaEndIf(merge);
}

Id EmitContext::HeapGuard(Id raw, u32 capacity, u32 entry) {
    // sotc: SOTC_HEAP guard (both modes). An index at or past the heap capacity names no descriptor: it is counted
    // (HeapDebugGuard) and recorded (kind 2), and the null slot 0 is used instead, so the shader reads no
    // out-of-range descriptor.
    const Id past{OpLogicalNot(U1[1], OpULessThan(U1[1], raw, ConstU32(capacity)))};
    HeapRecord(past, 2, entry, raw, raw, entry, Shader::HeapDebugGuard);
    return OpSelect(U32[1], past, u32_zero_value, raw);
}

Id EmitContext::LoadHeapImageIndex(u32 entry, u32 slot) {
    // sotc: SOTC_GCN_TEX: the probed slot of the image (HoistGcnTex), not the index table. A binding without a probed
    // slot (TableImage false) keeps the index table.
    if (profile.gcn_tex == 1 && entry < gcn_image_slots.size() && Sirit::ValidId(gcn_image_slots[entry])) {
        return gcn_image_slots[entry];
    }
    const Id raw{LoadTableIndex(entry)};
    if (profile.heap_images != 2) {
        return HeapGuard(raw, Shader::HeapImageCapacity, entry);
    }
    // sotc: SOTC_HEAP GPU verify. The classic slot is the index used (the rendering stays as the classic bind); the
    // table value is compared with it, and a difference is recorded (kind 0).
    const u32 rel = slot - Shader::GetBindlessImageRange(info.sw_stage).base;
    ASSERT_MSG(rel < Shader::HeapVerifyImageWords * 2, "heap verify: image slot outside the compact words");
    const Id classic{LoadHeapClassic(HeapVerifyFirstMember + HeapMemberShift(profile) +
                                         Shader::HeapVerifySamplerWords + rel / 2,
                                     (rel & 1) * 16)};
    HeapAdd(HeapDebugBase(), Shader::HeapDebugCompareImage, ConstU32(1u));
    HeapRecord(OpINotEqual(U1[1], classic, raw), 0, entry, classic, raw, entry,
               Shader::HeapDebugMismatchImage);
    return HeapGuard(classic, Shader::HeapImageCapacity, entry);
}

Id EmitContext::LoadHeapSamplerIndex(u32 entry, u32 index) {
    // sotc: SOTC_GCN_TEX: the probed slot of the sampler (HoistGcnTex), not the index table.
    if (profile.gcn_tex == 1 && index < gcn_sampler_slots.size()) {
        return gcn_sampler_slots[index];
    }
    const Id raw{LoadTableIndex(entry)};
    if (profile.heap_images != 2) {
        return HeapGuard(raw, Shader::HeapSamplerCapacity, entry);
    }
    // sotc: SOTC_HEAP GPU verify, as LoadHeapImageIndex (kind 1). Sampler s is compact sampler s.
    ASSERT_MSG(index < Shader::HeapVerifySamplerWords * 2, "heap verify: sampler outside the compact words");
    const Id classic{LoadHeapClassic(HeapVerifyFirstMember + HeapMemberShift(profile) + index / 2, (index & 1) * 16)};
    HeapAdd(HeapDebugBase(), Shader::HeapDebugCompareSampler, ConstU32(1u));
    HeapRecord(OpINotEqual(U1[1], classic, raw), 1, index, classic, raw, entry,
               Shader::HeapDebugMismatchSampler);
    return HeapGuard(classic, Shader::HeapSamplerCapacity, entry);
}

Id EmitContext::LoadSampler(u32 index) {
    if (uses_heap_images) {
        // sotc: SOTC_HEAP: sampler s is the entry after the images of the stage (UsesHeapImages).
        const Id heap_index{LoadHeapSamplerIndex(static_cast<u32>(info.images.size()) + index, index)};
        const Id sampler_ptr{OpAccessChain(sampler_pointer_type, bindless_sampler_array, heap_index)};
        // sotc: SOTC_GCN_TEX. A probed index is dynamically non-uniform: decorate the index, the access chain and the load.
        if (GcnSamplerProbed(index)) {
            Decorate(heap_index, spv::Decoration::NonUniform);
            Decorate(sampler_ptr, spv::Decoration::NonUniform);
            const Id sampler{OpLoad(sampler_type, sampler_ptr)};
            Decorate(sampler, spv::Decoration::NonUniform);
            return sampler;
        }
        return OpLoad(sampler_type, sampler_ptr);
    }
    if (!uses_bindless_samplers) {
        return OpLoad(sampler_type, samplers[index]);
    }
    // sotc: the heap index of this sampler is in its slot of the push constants (two per word).
    const u32 slot = bindless_sampler_base + index;
    const u32 word = PushData::SamplerSlotIndex + slot / 2;
    const u32 shift = (slot & 1) * 16;
    const Id ptr{OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U32[1]),
                               push_data_block, ConstU32(word))};
    const Id value{OpLoad(U32[1], ptr)};
    const Id heap_index{OpBitFieldUExtract(U32[1], value, ConstU32(shift), ConstU32(16U))};
    const Id sampler_ptr{OpAccessChain(sampler_pointer_type, bindless_sampler_array, heap_index)};
    return OpLoad(sampler_type, sampler_ptr);
}

Id EmitContext::LoadBindlessImageIndex(u32 slot) {
    // sotc: SOTC_N5_BINDLESS: two 16-bit slots per word, low half first (BindlessSamplerSlots).
    const u32 shift = (slot & 1) * 16;
    const Id ptr{OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U32[1]),
                               push_data_block, ConstU32(PushData::ImageSlotIndex + slot / 2))};
    const Id value{OpLoad(U32[1], ptr)};
    return OpBitFieldUExtract(U32[1], value, ConstU32(shift), ConstU32(16U));
}

void EmitContext::DefineSharedMemory() {
    const auto num_types = std::popcount(static_cast<u32>(info.shared_types));
    if (num_types == 0) {
        return;
    }
    ASSERT(info.hw_stage == HwStage::Compute);
    const u32 shared_memory_size =
        runtime_info.hw.cs.shared_memory_size + info.shared_memory_scratch_size;

    const auto make_type = [&](IR::Type type, Id element_type, u32 element_size,
                               std::string_view name) {
        if (False(info.shared_types & type)) {
            // Skip unused shared memory types.
            return std::make_tuple(Id{}, Id{}, Id{});
        }

        const u32 num_elements{Common::DivCeil(shared_memory_size, element_size)};
        const Id array_type{TypeArray(element_type, ConstU32(num_elements))};

        const auto mem_type = [&] {
            if (num_types > 1) {
                const Id struct_type{TypeStruct(array_type)};
                Decorate(struct_type, spv::Decoration::Block);
                MemberDecorate(struct_type, 0u, spv::Decoration::Offset, 0u);
                return struct_type;
            } else {
                return array_type;
            }
        }();

        const Id pointer = TypePointer(spv::StorageClass::Workgroup, mem_type);
        const Id element_pointer = TypePointer(spv::StorageClass::Workgroup, element_type);
        const Id variable = AddGlobalVariable(pointer, spv::StorageClass::Workgroup);
        Name(variable, name);
        interfaces.push_back(variable);

        if (num_types > 1) {
            Decorate(array_type, spv::Decoration::ArrayStride, element_size);
            Decorate(variable, spv::Decoration::Aliased);
        }

        return std::make_tuple(variable, element_pointer, pointer);
    };
    std::tie(shared_memory_u16, shared_u16, shared_memory_u16_type) =
        make_type(IR::Type::U16, U16, 2u, "shared_mem_u16");
    std::tie(shared_memory_u32, shared_u32, shared_memory_u32_type) =
        make_type(IR::Type::U32, U32[1], 4u, "shared_mem_u32");
    std::tie(shared_memory_u64, shared_u64, shared_memory_u64_type) =
        make_type(IR::Type::U64, U64, 8u, "shared_mem_u64");
}

Id EmitContext::DefineFloat32ToUfloatM5(u32 mantissa_bits, const std::string_view name) {
    // https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/util/format_r11g11b10f.h
    const auto func_type{TypeFunction(U32[1], F32[1])};
    const auto func{OpFunction(U32[1], spv::FunctionControlMask::MaskNone, func_type)};
    const auto value{OpFunctionParameter(F32[1])};
    Name(func, name);
    AddLabel();

    const auto raw_value{OpBitcast(U32[1], value)};
    const auto exponent{
        OpBitcast(S32[1], OpBitFieldSExtract(U32[1], raw_value, ConstU32(23U), ConstU32(8U)))};
    const auto sign{OpBitFieldUExtract(U32[1], raw_value, ConstU32(31U), ConstU32(1U))};

    const auto is_zero{OpLogicalOr(U1[1], OpIEqual(U1[1], raw_value, ConstU32(0U)),
                                   OpIEqual(U1[1], sign, ConstU32(1U)))};
    const auto is_nan{OpIsNan(U1[1], value)};
    const auto is_inf{OpIsInf(U1[1], value)};
    const auto is_denorm{OpSLessThanEqual(U1[1], exponent, ConstS32(-15))};

    const auto denorm_mantissa{OpConvertFToU(
        U32[1],
        OpRoundEven(F32[1], OpFMul(F32[1], value,
                                   ConstF32(static_cast<float>(1 << (mantissa_bits + 14))))))};
    const auto denorm_overflow{
        OpINotEqual(U1[1], OpShiftRightLogical(U32[1], denorm_mantissa, ConstU32(mantissa_bits)),
                    ConstU32(0U))};
    const auto denorm{
        OpSelect(U32[1], denorm_overflow, ConstU32(1U << mantissa_bits), denorm_mantissa)};

    const auto norm_mantissa{OpConvertFToU(
        U32[1],
        OpRoundEven(F32[1],
                    OpLdexp(F32[1], value,
                            OpISub(S32[1], ConstS32(static_cast<int>(mantissa_bits)), exponent))))};
    const auto norm_overflow{
        OpUGreaterThanEqual(U1[1], norm_mantissa, ConstU32(2U << mantissa_bits))};
    const auto norm_final_mantissa{OpBitwiseAnd(
        U32[1],
        OpSelect(U32[1], norm_overflow, OpShiftRightLogical(U32[1], norm_mantissa, ConstU32(1U)),
                 norm_mantissa),
        ConstU32((1U << mantissa_bits) - 1))};
    const auto norm_final_exponent{OpBitcast(
        U32[1],
        OpIAdd(S32[1],
               OpSelect(S32[1], norm_overflow, OpIAdd(S32[1], exponent, ConstS32(1)), exponent),
               ConstS32(15)))};
    const auto norm{OpBitFieldInsert(U32[1], norm_final_mantissa, norm_final_exponent,
                                     ConstU32(mantissa_bits), ConstU32(5U))};

    const auto result{OpSelect(U32[1], is_zero, ConstU32(0U),
                               OpSelect(U32[1], is_nan, ConstU32(31u << mantissa_bits | 1U),
                                        OpSelect(U32[1], is_inf, ConstU32(31U << mantissa_bits),
                                                 OpSelect(U32[1], is_denorm, denorm, norm))))};

    OpReturnValue(result);
    OpFunctionEnd();
    return func;
}

Id EmitContext::DefineUfloatM5ToFloat32(u32 mantissa_bits, const std::string_view name) {
    // https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/util/format_r11g11b10f.h
    const auto func_type{TypeFunction(F32[1], U32[1])};
    const auto func{OpFunction(F32[1], spv::FunctionControlMask::MaskNone, func_type)};
    const auto value{OpFunctionParameter(U32[1])};
    Name(func, name);
    AddLabel();

    const Id exponent{OpBitFieldUExtract(U32[1], value, ConstU32(mantissa_bits), ConstU32(5U))};
    const Id mantissa{OpBitFieldUExtract(U32[1], value, ConstU32(0U), ConstU32(mantissa_bits))};
    const Id mantissa_f{OpConvertUToF(F32[1], mantissa)};
    const Id a{OpSelect(F32[1], OpINotEqual(U1[1], mantissa, u32_zero_value),
                        OpFMul(F32[1], ConstF32(1.f / (1 << (14 + mantissa_bits))), mantissa_f),
                        f32_zero_value)};
    const Id b{OpBitcast(F32[1], OpBitwiseOr(U32[1], mantissa, ConstU32(0x7f800000U)))};
    const Id exponent_c{OpISub(U32[1], exponent, ConstU32(15U))};
    const Id scale_a{
        OpFDiv(F32[1], ConstF32(1.f),
               OpConvertUToF(F32[1], OpShiftLeftLogical(U32[1], u32_one_value,
                                                        OpSNegate(U32[1], exponent_c))))};
    const Id scale_b{OpConvertUToF(F32[1], OpShiftLeftLogical(U32[1], u32_one_value, exponent_c))};
    const Id scale{
        OpSelect(F32[1], OpSLessThan(U1[1], exponent_c, u32_zero_value), scale_a, scale_b)};
    const Id c{OpFMul(F32[1], scale,
                      OpFAdd(F32[1], ConstF32(1.f),
                             OpFDiv(F32[1], mantissa_f, ConstF32(f32(1 << mantissa_bits)))))};
    const Id result{OpSelect(F32[1], OpIEqual(U1[1], exponent, u32_zero_value), a,
                             OpSelect(F32[1], OpIEqual(U1[1], exponent, ConstU32(31U)), b, c))};
    OpReturnValue(result);
    OpFunctionEnd();
    return func;
}

Id EmitContext::DefineGetBdaPointer() {
    const auto caching_pagebits{Constant(U64, static_cast<u64>(profile.sparse_page_shift))};
    const auto caching_pagemask{
        Constant(U64, static_cast<u64>((1u << profile.sparse_page_shift) - 1))};

    const auto func_type{TypeFunction(U64, U64)};
    const auto func{OpFunction(U64, spv::FunctionControlMask::MaskNone, func_type)};
    const auto address{OpFunctionParameter(U64)};
    Name(func, "get_bda_pointer");
    AddLabel();

    const auto fault_label{OpLabel()};
    const auto available_label{OpLabel()};
    const auto merge_label{OpLabel()};

    // Get page BDA
    const auto page{OpShiftRightLogical(U64, address, caching_pagebits)};
    const auto page32{OpUConvert(U32[1], page)};
    const auto& bda_buffer{buffers[bda_pagetable_index]};
    const auto [bda_buffer_id, bda_pointer_type] = bda_buffer.Alias(PointerType::U64);
    const auto bda_ptr{OpAccessChain(bda_pointer_type, bda_buffer_id, u32_zero_value, page32)};
    const auto bda{OpLoad(U64, bda_ptr)};

    // Check if page is GPU cached
    const auto is_fault{OpIEqual(U1[1], bda, u64_zero_value)};
    OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    OpBranchConditional(is_fault, fault_label, available_label);

    // First time acces, mark as fault
    AddLabel(fault_label);
    const auto& fault_buffer{buffers[fault_buffer_index]};
    const auto [fault_buffer_id, fault_pointer_type] = fault_buffer.Alias(PointerType::U32);
    const auto page_div32{OpShiftRightLogical(U32[1], page32, ConstU32(5U))};
    const auto page_mod32{OpBitwiseAnd(U32[1], page32, ConstU32(31U))};
    const auto page_mask{OpShiftLeftLogical(U32[1], u32_one_value, page_mod32)};
    const auto fault_ptr{
        OpAccessChain(fault_pointer_type, fault_buffer_id, u32_zero_value, page_div32)};
    const auto fault_value{OpLoad(U32[1], fault_ptr)};
    const auto fault_value_masked{OpBitwiseOr(U32[1], fault_value, page_mask)};
    OpStore(fault_ptr, fault_value_masked);

    // Return null pointer
    const auto fallback_result{u64_zero_value};
    OpBranch(merge_label);

    // Value is available, compute address
    AddLabel(available_label);
    const auto offset_in_bda{OpBitwiseAnd(U64, address, caching_pagemask)};
    const auto addr{OpIAdd(U64, bda, offset_in_bda)};
    OpBranch(merge_label);

    // Merge
    AddLabel(merge_label);
    const auto result{OpPhi(U64, addr, available_label, fallback_result, fault_label)};
    OpReturnValue(result);
    OpFunctionEnd();
    return func;
}

Id EmitContext::DefineReadConst(bool dynamic) {
    const auto func_type{!dynamic ? TypeFunction(U32[1], U32[2], U32[1], U32[1])
                                  : TypeFunction(U32[1], U32[2], U32[1])};
    const auto func{OpFunction(U32[1], spv::FunctionControlMask::MaskNone, func_type)};
    const auto base{OpFunctionParameter(U32[2])};
    const auto offset{OpFunctionParameter(U32[1])};
    const auto flatbuf_offset{!dynamic ? OpFunctionParameter(U32[1]) : Id{}};
    Name(func, dynamic ? "read_const_dynamic" : "read_const");
    AddLabel();

    const auto base_lo{OpUConvert(U64, OpCompositeExtract(U32[1], base, 0))};
    const auto base_hi{OpUConvert(U64, OpCompositeExtract(U32[1], base, 1))};
    const auto base_shift{OpShiftLeftLogical(U64, base_hi, ConstU32(32U))};
    const auto base_addr{OpBitwiseOr(U64, base_lo, base_shift)};
    const auto offset_bytes{OpShiftLeftLogical(U32[1], offset, ConstU32(2U))};
    const auto addr{OpIAdd(U64, base_addr, OpUConvert(U64, offset_bytes))};

    const auto result = EmitDwordMemoryRead(addr, [&]() {
        if (dynamic) {
            return u32_zero_value;
        } else {
            return EmitFlatbufferLoad(flatbuf_offset);
        }
    });

    OpReturnValue(result);
    OpFunctionEnd();
    return func;
}

// sotc: the hardware LOD counters (sotc_lod_stats.h) from the sample's derivatives. lambda =
// log2 of the longest texel footprint axis over the T#'s mip 0 size (isotropic: an anisotropic
// sample may read a finer mip, which is the bias knob's job on the CPU side). The finest mip of
// every enabled bank (T# dword 6: counter_bank_id 12-19, lod_hw_cnt_en 20) is kept with an atomic
// min, skipped when the bank already holds this mip or a finer one (one load, few atomics).
// Word 256 of the buffer says whether this frame measures at all (Rasterizer::
// SotcLodFeedbackCycle measures one cycle in four): a draw-uniform branch, so the derivatives
// below stay in uniform control flow; measuring every frame cost ~15% of the frame rate.
Id EmitContext::DefineLodFeedback() {
    const auto func_type{TypeFunction(void_id, U32[1], U32[1], U32[1], F32[2])};
    const auto func{OpFunction(void_id, spv::FunctionControlMask::MaskNone, func_type)};
    const auto dw2{OpFunctionParameter(U32[1])};
    const auto dw3{OpFunctionParameter(U32[1])};
    const auto dw6{OpFunctionParameter(U32[1])};
    const auto uv{OpFunctionParameter(F32[2])};
    Name(func, "lod_feedback");
    AddLabel();

    const auto measure_label{OpLabel()};
    const auto write_label{OpLabel()};
    const auto merge_label{OpLabel()};
    const auto end_label{OpLabel()};

    const auto& feedback{buffers[lod_feedback_index]};
    const auto [feedback_id, feedback_pointer_type] = feedback.Alias(PointerType::U32);
    const auto gate_ptr{
        OpAccessChain(feedback_pointer_type, feedback_id, u32_zero_value, ConstU32(256U))};
    const auto measuring{OpINotEqual(U1[1], OpLoad(U32[1], gate_ptr), u32_zero_value)};
    OpSelectionMerge(end_label, spv::SelectionControlMask::MaskNone);
    OpBranchConditional(measuring, measure_label, end_label);

    AddLabel(measure_label);
    const auto ddx{OpDPdx(F32[2], uv)};
    const auto ddy{OpDPdy(F32[2], uv)};

    const auto enabled{OpINotEqual(U1[1], OpBitwiseAnd(U32[1], dw6, ConstU32(1U << 20)),
                                   u32_zero_value)};
    const auto bank{OpBitwiseAnd(U32[1], OpShiftRightLogical(U32[1], dw6, ConstU32(12U)),
                                 ConstU32(0xffU))};
    const auto width{OpConvertUToF(
        F32[1], OpIAdd(U32[1], OpBitwiseAnd(U32[1], dw2, ConstU32(0x3fffU)), u32_one_value))};
    const auto height{OpConvertUToF(
        F32[1], OpIAdd(U32[1],
                       OpBitwiseAnd(U32[1], OpShiftRightLogical(U32[1], dw2, ConstU32(14U)),
                                    ConstU32(0x3fffU)),
                       u32_one_value))};
    const auto size{OpCompositeConstruct(F32[2], width, height)};
    const auto tx{OpFMul(F32[2], ddx, size)};
    const auto ty{OpFMul(F32[2], ddy, size)};
    const auto footprint{
        OpFMax(F32[1], OpFMax(F32[1], OpDot(F32[1], tx, tx), OpDot(F32[1], ty, ty)),
               ConstF32(1e-20f))};
    const auto lambda{OpFMul(F32[1], OpLog2(F32[1], footprint), ConstF32(0.5f))};
    // NaN derivatives (helper lanes at triangle edges) report nothing
    const auto valid{OpLogicalAnd(U1[1], enabled, OpLogicalNot(U1[1], OpIsNan(U1[1], lambda)))};
    // Never coarser than the texture's last mip (T# dword 3 bits 16-19): the game waits, while
    // loading, until every bank's mip is resident, and a mip the texture lacks never is (loading
    // screens of ~100 s instead of ~1 s, 22 Oct).
    const auto last_level{OpConvertUToF(
        F32[1], OpBitwiseAnd(U32[1], OpShiftRightLogical(U32[1], dw3, ConstU32(16U)),
                             ConstU32(0xfU)))};
    const auto mip_f{OpFClamp(F32[1], OpFloor(F32[1], lambda), f32_zero_value, last_level)};
    const auto mip{OpConvertFToU(U32[1], mip_f)};

    const auto ptr{OpAccessChain(feedback_pointer_type, feedback_id, u32_zero_value, bank)};
    const auto finer{OpULessThan(U1[1], mip, OpLoad(U32[1], ptr))};
    const auto write{OpLogicalAnd(U1[1], valid, finer)}; // before the merge: last two in block
    OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    OpBranchConditional(write, write_label, merge_label);

    AddLabel(write_label);
    OpAtomicUMin(U32[1], ptr, ConstU32(static_cast<u32>(spv::Scope::Device)), u32_zero_value,
                 mip);
    OpBranch(merge_label);

    AddLabel(merge_label);
    OpBranch(end_label);

    AddLabel(end_label);
    OpReturn();
    OpFunctionEnd();
    return func;
}

void EmitContext::DefineFunctions() {
    if (info.uses_pack_10_11_11) {
        f32_to_uf11 = DefineFloat32ToUfloatM5(6, "f32_to_uf11");
        f32_to_uf10 = DefineFloat32ToUfloatM5(5, "f32_to_uf10");
    }
    if (info.uses_unpack_10_11_11) {
        uf11_to_f32 = DefineUfloatM5ToFloat32(6, "uf11_to_f32");
        uf10_to_f32 = DefineUfloatM5ToFloat32(5, "uf10_to_f32");
    }
    if (info.uses_dma) {
        get_bda_pointer = DefineGetBdaPointer();
    }
    if (info.uses_lod_feedback) {
        lod_feedback = DefineLodFeedback();
    }
    // sotc: SOTC_SPV_FUNCS: the generic formatted buffer decode, one function per shader (set by LowerBufferFormatToRaw).
    if (info.uses_generic_format_decode) {
        generic_format_func = DefineGenericBufferFormat();
    }
    // sotc: SOTC_GCN_BUF: the page-miss record as one function, called from every table access (TableAddresses).
    if (profile.gcn_buf != 0 && GcnMissEmit() && ShaderAddrTableMode(profile) && UsesShaderAddr(profile, info)) {
        // sotc: gcn_page_miss(page U64, flags U32, guest_base U64, block_size U32) (GcnPageMiss).
        const Id func_type{TypeFunction(void_id, U64, U32[1], U64, U32[1])};
        gcn_page_miss_func = OpFunction(void_id, spv::FunctionControlMask::DontInline, func_type);
        const Id page{OpFunctionParameter(U64)};
        const Id miss_flags{OpFunctionParameter(U32[1])};
        const Id miss_base{OpFunctionParameter(U64)};
        const Id miss_size{OpFunctionParameter(U32[1])};
        Name(gcn_page_miss_func, "gcn_page_miss");
        AddLabel();
        GcnPageMiss(page, miss_flags, miss_base, miss_size);
        OpReturn();
        OpFunctionEnd();
    }
    // sotc: SOTC_SPV_FUNCS. The element addresses and validity of a table-mode load as one function (BdaLoadTable
    // calls it). It computes what TableAddresses computes with elem_valid, for count 1..4 and elem_size 1 << elem_shift:
    // the same first and last page lookups, the same page-miss call, and per element the same address and validity.
    // Lanes i >= count are invalid (ok 0, address = table_device). count and elem_shift are run-time values, so one
    // body serves every load: fewer instructions per load site, and no inline copy per site.
    if (SpvFuncs() && ShaderAddrTableMode(profile) && UsesShaderAddr(profile, info) && Sirit::ValidId(U64)) {
        gcn_table_u64x4 = TypeVector(U64, 4);
        gcn_table_addr_result = TypeStruct(gcn_table_u64x4, U32[4]);
        // sotc: the last parameter is the page-miss flags of the calling access (GcnPageMissFlags).
        const Id func_type{TypeFunction(gcn_table_addr_result, U64, U32[1], U32[1], U32[1], U32[1], U32[1])};
        gcn_table_addr_func = OpFunction(gcn_table_addr_result, spv::FunctionControlMask::DontInline, func_type);
        const Id guest_base{OpFunctionParameter(U64)};
        const Id block_size{OpFunctionParameter(U32[1])};
        const Id index{OpFunctionParameter(U32[1])};
        const Id elem_shift{OpFunctionParameter(U32[1])};
        const Id count{OpFunctionParameter(U32[1])};
        const Id miss_flags{OpFunctionParameter(U32[1])};
        Name(gcn_table_addr_func, "gcn_table_addr");
        AddLabel();

        const Id size64{OpShiftLeftLogical(U64, u64_one_value, OpUConvert(U64, elem_shift))};
        const Id size_mask{OpNot(U64, OpISub(U64, size64, u64_one_value))};
        const Id num_elements{OpShiftRightLogical(U32[1], block_size, elem_shift)};
        const Id first_in{OpULessThan(U1[1], index, num_elements)};
        const Id first_index{OpSelect(U32[1], first_in, index, u32_zero_value)};
        // The last element that is still in the block, at most count - 1 after the first one (as TableAddresses).
        const Id last_in_block{
            OpISub(U32[1], OpSelect(U32[1], first_in, num_elements, u32_one_value), u32_one_value)};
        const Id wanted{OpIAdd(U32[1], first_index, OpISub(U32[1], count, u32_one_value))};
        const Id last_index_used{
            OpSelect(U32[1], OpULessThan(U1[1], wanted, last_in_block), wanted, last_in_block)};

        const auto push_word = [&](u32 member) {
            return OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64),
                                             push_data_block, ConstU32(shader_addr_member + member)));
        };
        const Id window_guest{push_word(0)};
        const Id table_device{push_word(1)};

        const auto guest_of = [&](Id elem_index) {
            return OpBitwiseAnd(
                U64,
                OpIAdd(U64, guest_base, OpIMul(U64, OpUConvert(U64, elem_index), size64)),
                size_mask);
        };
        // An access outside the block uses page 0 (its entry load stays inside the table), as TableAddresses.
        const auto entry_of = [&](Id guest) {
            const Id rel{OpSelect(U64, first_in, OpISub(U64, guest, window_guest), u64_zero_value)};
            const Id page{OpShiftRightLogical(U64, rel, Constant(U64, 14ULL))};
            const Id entry_address{OpIAdd(U64, table_device, OpIMul(U64, page, Constant(U64, 8ULL)))};
            return OpLoad(U64, BdaPointer(entry_address, U64), spv::MemoryAccessMask::Aligned, 8u);
        };
        const Id page_mask{Constant(U64, ~((1ULL << 14) - 1))};
        const Id guest_first{guest_of(first_index)};
        const Id entry_first{entry_of(guest_first)};
        const Id guest_last{guest_of(last_index_used)};
        const Id entry_last{entry_of(guest_last)};
        const Id page_first{OpBitwiseAnd(U64, guest_first, page_mask)};

        // sotc: SOTC_GCN_BUF: the same page-miss record as TableAddresses (one call, first missing page).
        if (profile.gcn_buf != 0 && GcnMissEmit() && Sirit::ValidId(gcn_page_miss_func)) {
            const Id first_missing{OpIEqual(U1[1], entry_first, u64_zero_value)};
            const Id any_missing{OpLogicalOr(U1[1], first_missing, OpIEqual(U1[1], entry_last, u64_zero_value))};
            const Id miss{OpLogicalAnd(U1[1], first_in, any_missing)};
            const Id miss_page{
                OpSelect(U64, first_missing, page_first, OpBitwiseAnd(U64, guest_last, page_mask))};
            const Id miss_merge{BdaBeginIf(miss)};
            OpFunctionCall(void_id, gcn_page_miss_func, miss_page, miss_flags, guest_base, block_size);
            BdaEndIf(miss_merge);
        }

        std::array<Id, 4> addresses{};
        std::array<Id, 4> oks{};
        for (u32 i = 0; i < 4; i++) {
            // Element i is in the block (index + i < num_elements) and inside the access (i < count).
            const Id active{OpULessThan(U1[1], ConstU32(i), count)};
            const Id in_block{OpLogicalAnd(U1[1], OpUGreaterThan(U1[1], num_elements, ConstU32(i)),
                                           OpULessThanEqual(U1[1], index,
                                                            OpISub(U32[1], num_elements, ConstU32(i + 1))))};
            const Id elem_in{OpLogicalAnd(U1[1], active, in_block)};
            const Id guest{i == 0 ? guest_first
                                  : guest_of(OpSelect(U32[1], elem_in, OpIAdd(U32[1], index, ConstU32(i)),
                                                      first_index))};
            const Id entry{i == 0 ? entry_first
                                  : OpSelect(U64, OpIEqual(U1[1], OpBitwiseAnd(U64, guest, page_mask), page_first),
                                             entry_first, entry_last)};
            const Id elem_address{OpIAdd(U64, entry, OpBitwiseAnd(U64, guest, Constant(U64, (1ULL << 14) - 1)))};
            const Id ok{OpLogicalAnd(U1[1], elem_in, OpINotEqual(U1[1], entry, u64_zero_value))};
            addresses[i] = OpSelect(U64, ok, elem_address, table_device);
            oks[i] = OpSelect(U32[1], ok, u32_one_value, u32_zero_value);
        }
        const Id addresses_vec{OpCompositeConstruct(gcn_table_u64x4, addresses)};
        const Id oks_vec{OpCompositeConstruct(U32[4], oks)};
        const Id result{OpCompositeConstruct(gcn_table_addr_result, std::array<Id, 2>{addresses_vec, oks_vec})};
        OpReturnValue(result);
        OpFunctionEnd();
    }

    if (True(info.readconst_types & Info::ReadConstType::Immediate)) {
        LOG_DEBUG(Render_Recompiler, "Shader {:#x} uses immediate ReadConst", info.pgm_hash);
        read_const = DefineReadConst(false);
    }
    if (True(info.readconst_types & Info::ReadConstType::Dynamic)) {
        LOG_DEBUG(Render_Recompiler, "Shader {:#x} uses dynamic ReadConst", info.pgm_hash);
        read_const_dynamic = DefineReadConst(true);
    }
}

} // namespace Shader::Backend::SPIRV
