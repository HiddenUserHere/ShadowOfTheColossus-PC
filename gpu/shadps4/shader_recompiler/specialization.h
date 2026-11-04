// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bitset>
#include <cstdlib>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Shader {

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization&) const = default;
};

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    bool operator==(const BufferSpecialization& other) const {
        return stride == other.stride && is_formatted == other.is_formatted &&
               swizzle_enable == other.swizzle_enable &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion)) &&
               (!swizzle_enable ||
                (index_stride == other.index_stride && element_size == other.element_size));
    }
};

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // FIXME any pipeline cache changes needed?
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization&) const = default;
};

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization&) const = default;
};

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

// sotc: SOTC_SPEC_MEMO. The element fills below are shared by the StageSpecialization constructor
// and by BuildSpecKey, so both write the same values for the same sharp. An element is filled only
// when its sharp is present; otherwise it keeps the zero value (the constructor's emplace_back).

inline void FillVsAttrib(VsAttribSpecialization& spec, const Gcn::VertexAttribute& desc,
                         AmdGpu::Buffer sharp, const RuntimeInfo& runtime_info) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (const auto step_rate = desc.GetStepRate(); step_rate != InstanceIdType::None) {
        spec.divisor = step_rate == InstanceIdType::OverStepRate0
                           ? runtime_info.sw.vs.step_rate_0
                           : (step_rate == InstanceIdType::OverStepRate1
                                  ? runtime_info.sw.vs.step_rate_1
                                  : 1);
    }
    spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
    spec.dst_select = sharp.DstSelect();
}

inline void FillBuffer(BufferSpecialization& spec, const BufferResource& desc,
                       AmdGpu::Buffer sharp) {
    spec.stride = sharp.GetStride();
    spec.is_formatted = desc.is_formatted;
    spec.swizzle_enable = sharp.swizzle_enable;
    if (spec.is_formatted) {
        spec.data_format = static_cast<u32>(sharp.GetDataFmt());
        spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
        spec.dst_select = sharp.DstSelect();
        spec.num_conversion = sharp.GetNumberConversion();
    }
    if (spec.swizzle_enable) {
        spec.index_stride = sharp.index_stride;
        spec.element_size = sharp.element_size;
    }
}

// sotc: SOTC_IMAGE_SRGB_NORMALIZE (on unless =0). An image's sRGB number format reaches the translation only
// through a sampler's force_degamma (resource_patching_pass.cpp, the degamma of a non-sRGB image); the sRGB
// conversion itself is the image view's format. Without force_degamma in the stage the flag was a key difference
// without a code difference (27% of the game's sampled images), and it cannot be known before the draw: it is keyed
// false. KeepSrgb is evaluated once per key, before the images are filled.
inline bool SrgbNormalizeOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_IMAGE_SRGB_NORMALIZE");
        return !v || v[0] != '0';
    }();
    return on;
}

inline bool KeepSrgb(const Info& info) {
    if (!SrgbNormalizeOn()) {
        return true;
    }
    for (const auto& desc : info.samplers) {
        if (const auto sharp = desc.GetSharp(info); sharp && sharp.force_degamma) {
            return true;
        }
    }
    return false;
}

inline void FillImage(ImageSpecialization& spec, const ImageResource& desc, AmdGpu::Image sharp,
                      const Info& info, bool keep_srgb = true) {
    spec.type = sharp.GetViewType(desc.is_array);
    spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
    spec.is_storage = desc.is_written;
    if (spec.is_storage) {
        spec.dst_select = sharp.DstSelect();
    } else {
        spec.is_srgb = keep_srgb && sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
    }
    spec.num_conversion = sharp.GetNumberConversion();
    spec.num_bindings = desc.NumBindings(info);
}

inline void FillFMask(FMaskSpecialization& spec, AmdGpu::Image sharp) {
    spec.width = sharp.width;
    spec.height = sharp.height;
}

inline void FillSampler(SamplerSpecialization& spec, AmdGpu::Sampler sharp) {
    spec.force_unnormalized = sharp.force_unnormalized;
    spec.force_degamma = sharp.force_degamma;
}

// sotc: SOTC_SPEC_MEMO. Key words: every field of an element, in a fixed order and width. The
// words are written field by field, so struct padding never reaches a key.
inline void PackMapping(std::vector<u32>& out, const AmdGpu::CompMapping& m) {
    out.push_back(static_cast<u32>(m.array[0]) | (static_cast<u32>(m.array[1]) << 8) |
                  (static_cast<u32>(m.array[2]) << 16) | (static_cast<u32>(m.array[3]) << 24));
}

inline void PackVsAttrib(std::vector<u32>& out, const VsAttribSpecialization& s) {
    out.push_back(s.divisor);
    out.push_back(static_cast<u32>(s.num_class));
    PackMapping(out, s.dst_select);
}

inline void PackBuffer(std::vector<u32>& out, const BufferSpecialization& s) {
    out.push_back(s.stride);
    out.push_back(s.is_formatted);
    out.push_back(s.swizzle_enable);
    out.push_back(s.data_format);
    out.push_back(s.num_format);
    out.push_back(s.index_stride);
    out.push_back(s.element_size);
    PackMapping(out, s.dst_select);
    out.push_back(static_cast<u32>(s.num_conversion));
}

inline void PackImage(std::vector<u32>& out, const ImageSpecialization& s) {
    const auto type = static_cast<u64>(s.type);
    out.push_back(static_cast<u32>(type));
    out.push_back(static_cast<u32>(type >> 32));
    out.push_back(u32{s.is_integer});
    out.push_back(u32{s.is_storage});
    out.push_back(u32{s.is_srgb});
    PackMapping(out, s.dst_select);
    out.push_back(static_cast<u32>(s.num_conversion));
    out.push_back(s.num_bindings);
}

inline void PackFMask(std::vector<u32>& out, const FMaskSpecialization& s) {
    out.push_back(s.width);
    out.push_back(s.height);
}

inline void PackSampler(std::vector<u32>& out, const SamplerSpecialization& s) {
    out.push_back(u32{s.force_unnormalized});
    out.push_back(u32{s.force_degamma});
}

/**
 * Alongside runtime information, this structure also checks bound resources
 * for compatibility. Can be used as a key for storing shader permutations.
 * Is separate from runtime information, because resource layout can only be deduced
 * after the first compilation of a module.
 */
struct StageSpecialization {
    static constexpr size_t MaxStageResources = 128;

    const Info* info{};
    RuntimeInfo runtime_info{};
    std::bitset<MaxStageResources> bitset{};
    Gcn::FetchShaderData fetch_shader_data{};
    SmallVector<VsAttribSpecialization, 32> vs_attribs;
    SmallVector<BufferSpecialization, 16> buffers;
    SmallVector<ImageSpecialization, 16> images;
    SmallVector<FMaskSpecialization, 8> fmasks;
    SmallVector<SamplerSpecialization, 16> samplers;
    Backend::Bindings start{};

    StageSpecialization() = default;
    // sotc: runtime info by reference (one copy into the member instead of three per stage).
    StageSpecialization(const Info& info_, const RuntimeInfo& runtime_info_, const Profile& profile_,
                        Backend::Bindings start_)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        if (info_.sw_stage == SwStage::Vertex && Gcn::ParseFetchShader(info_, fetch_shader_data)) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data.attributes,
                         [this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             FillVsAttrib(spec, desc, sharp, runtime_info);
                         });
        }
        u32 binding{};
        ForEachSharp(binding, buffers, info->buffers,
                     [](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                         FillBuffer(spec, desc, sharp);
                     });
        const bool keep_srgb = info->images.empty() || KeepSrgb(*info); // sotc: SOTC_IMAGE_SRGB_NORMALIZE
        ForEachSharp(binding, images, info->images,
                     [&](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         FillImage(spec, desc, sharp, *info, keep_srgb);
                     });
        ForEachSharp(binding, fmasks, info->fmasks,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         FillFMask(spec, sharp);
                     });
        ForEachSharp(samplers, info->samplers,
                     [](auto& spec, const auto& desc, AmdGpu::Sampler sharp) {
                         FillSampler(spec, sharp);
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->sw_stage == SwStage::TessellationControl ||
            info->sw_stage == SwStage::TessellationEval) {
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            runtime_info.InitFromTessConstants(tess_constants);
        }
    }

    void ForEachSharp(auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                binding++;
                continue;
            }
            bitset[binding++] = true;
            func(spec, desc, sharp);
        }
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    bool operator==(const StageSpecialization& other) const {
        if (!Valid()) {
            return false;
        }

        if (vs_attribs != other.vs_attribs) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (fetch_shader_data != other.fetch_shader_data) {
            return false;
        }

        if (fmasks != other.fmasks) {
            return false;
        }

        // For VS which only generates geometry and doesn't have any inputs, its start
        // bindings still may change as they depend on previously processed FS. The check below
        // handles this case and prevents generation of redundant permutations. This is also safe
        // for other types of shaders with no bindings.
        if (buffers.empty() && images.empty() && samplers.empty()) {
            return true;
        }

        if (start != other.start) {
            return false;
        }

        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return false;
            }
        }

        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

// sotc: SOTC_SPEC_MEMO. Packs the stored specialization of a permutation into key words, in the
// same layout BuildSpecKey writes: the element count of each list, then per element its presence
// bit (buffers, images, fmasks: the bitset, which the constructor sets per binding) and its fields.
inline void PackSpecObject(std::vector<u32>& out, const StageSpecialization& spec) {
    out.clear();
    out.push_back(static_cast<u32>(spec.vs_attribs.size()));
    for (size_t i = 0; i < spec.vs_attribs.size(); ++i) {
        PackVsAttrib(out, spec.vs_attribs[i]);
    }
    u32 binding = 0;
    out.push_back(static_cast<u32>(spec.buffers.size()));
    for (size_t i = 0; i < spec.buffers.size(); ++i) {
        out.push_back(u32{spec.bitset[binding++]});
        PackBuffer(out, spec.buffers[i]);
    }
    out.push_back(static_cast<u32>(spec.images.size()));
    for (size_t i = 0; i < spec.images.size(); ++i) {
        out.push_back(u32{spec.bitset[binding++]});
        PackImage(out, spec.images[i]);
    }
    out.push_back(static_cast<u32>(spec.fmasks.size()));
    for (size_t i = 0; i < spec.fmasks.size(); ++i) {
        out.push_back(u32{spec.bitset[binding++]});
        PackFMask(out, spec.fmasks[i]);
    }
    out.push_back(static_cast<u32>(spec.samplers.size()));
    for (size_t i = 0; i < spec.samplers.size(); ++i) {
        PackSampler(out, spec.samplers[i]);
    }
}

// sotc: SOTC_SPEC_MEMO. One pass over the sharps of a stage, through the same fills as the
// StageSpecialization constructor, packed into the key words PackSpecObject also produces for the
// constructed specialization. The vertex fetch shader is parsed into fetch as the constructor does
// (the caller compares it with the stored one); runtime_info and the tessellation constants are
// the caller's to compare. Reads guest memory only through the sharp accessors.
inline void BuildSpecKey(std::vector<u32>& out, const Info& info,
                         const RuntimeInfo& runtime_info, Gcn::FetchShaderData& fetch) {
    out.clear();
    fetch = Gcn::FetchShaderData{};
    if (info.sw_stage == SwStage::Vertex && Gcn::ParseFetchShader(info, fetch)) {
        out.push_back(static_cast<u32>(fetch.attributes.size()));
        for (const auto& desc : fetch.attributes) {
            VsAttribSpecialization spec{};
            const auto sharp = desc.GetSharp(info);
            if (sharp) {
                FillVsAttrib(spec, desc, sharp, runtime_info);
            }
            PackVsAttrib(out, spec);
        }
    } else {
        out.push_back(0);
    }

    out.push_back(static_cast<u32>(info.buffers.size()));
    for (const auto& desc : info.buffers) {
        BufferSpecialization spec{};
        const auto sharp = desc.GetSharp(info);
        out.push_back(sharp ? 1u : 0u);
        if (sharp) {
            FillBuffer(spec, desc, sharp);
        }
        PackBuffer(out, spec);
    }

    out.push_back(static_cast<u32>(info.images.size()));
    const bool keep_srgb = info.images.empty() || KeepSrgb(info); // sotc: SOTC_IMAGE_SRGB_NORMALIZE
    for (const auto& desc : info.images) {
        ImageSpecialization spec{};
        const auto sharp = desc.GetSharp(info);
        out.push_back(sharp ? 1u : 0u);
        if (sharp) {
            FillImage(spec, desc, sharp, info, keep_srgb);
        }
        PackImage(out, spec);
    }

    out.push_back(static_cast<u32>(info.fmasks.size()));
    for (const auto& desc : info.fmasks) {
        FMaskSpecialization spec{};
        const auto sharp = desc.GetSharp(info);
        out.push_back(sharp ? 1u : 0u);
        if (sharp) {
            FillFMask(spec, sharp);
        }
        PackFMask(out, spec);
    }

    out.push_back(static_cast<u32>(info.samplers.size()));
    for (const auto& desc : info.samplers) {
        SamplerSpecialization spec{};
        const auto sharp = desc.GetSharp(info);
        if (sharp) {
            FillSampler(spec, sharp);
        }
        PackSampler(out, spec);
    }
}

} // namespace Shader
