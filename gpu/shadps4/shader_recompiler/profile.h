// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader {

struct Profile {
    u32 max_viewport_width{};
    u32 max_viewport_height{};
    u32 max_shared_memory_size{};
    u32 supported_spirv{0x00010000};
    u32 subgroup_size{};
    u32 sparse_page_shift{};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_float16{};
    bool support_float64{};
    bool supports_denorm_behavior_independence{};
    bool supports_rounding_mode_independence{};
    bool support_fp16_denorm_preserve{};
    bool support_fp16_denorm_flush{};
    bool support_fp16_round_to_zero{};
    bool support_fp32_denorm_preserve{};
    bool support_fp32_denorm_flush{};
    bool support_fp32_round_to_zero{};
    bool support_fp64_denorm_preserve{};
    bool support_fp64_denorm_flush{};
    bool support_fp64_round_to_zero{};
    bool support_fp16_signed_zero_inf_nan_preserve{};
    bool support_fp32_signed_zero_inf_nan_preserve{};
    bool support_fp64_signed_zero_inf_nan_preserve{};
    bool supports_image_load_store_lod{};
    bool supports_native_cube_calc{};
    bool supports_trinary_minmax{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
    bool supports_buffer_int64_atomics{};
    bool supports_shared_int64_atomics{};
    bool supports_workgroup_explicit_memory_layout{};
    bool supports_amd_shader_explicit_vertex_parameter{};
    bool supports_fragment_shader_barycentric{};
    bool supports_shader_subgroup_clock{};
    bool has_broken_spirv_clamp{};
    bool lower_left_origin_mode{};
    bool needs_manual_interpolation{};
    bool needs_lds_barriers{};
    bool needs_buffer_offsets{};
    bool needs_unorm_fixup{};
    bool needs_clip_distance_emulation{};
    bool supports_shader_stencil_export{};
    /// sotc: supports_fragment_shader_barycentric emulated by an auxiliary geometry shader
    /// (EmitBarycentricGeometryShader): per-vertex inputs and barycentrics become plain inputs.
    /// Last, so the profiles stored in older caches still compare equal.
    bool barycentric_via_gs{};
    /// sotc: SOTC_STAGE_BINDINGS: each graphics stage in its own descriptor set (see
    /// StageDescriptorSet), bindings numbered from 0 in every stage. Sits in the padding byte
    /// after barycentric_via_gs, so the profile keeps its size; the cache check normalizes it.
    bool stage_bindings{};
    /// sotc: SOTC_BDA: guest buffers that are neither texel nor coherent are read and written
    /// through physical addresses from BufferType::BdaBlockTable, not through descriptors. Last
    /// member: it grows the profile to 68 bytes, so the cache check reads a 64-byte profile from
    /// an older cache as this flag off (vk_pipeline_serialization.cpp).
    bool bda_buffers{};
    /// sotc: SOTC_N5_BINDLESS: sampled images come from the bindless heap (BindlessImageKeyOf).
    /// Last member: the profile grows from 68 to 72 bytes (alignment 4). vk_pipeline_serialization.cpp
    /// pads the 68-byte profiles of older caches with this byte off, the 64-byte ones through bda.
    bool bindless_images{};
    /// sotc: SOTC_HEAP: a stage whose images and samplers are all in the bindless heap takes their heap
    /// indices from the index table of the draw (UsesHeapImages), not from push slots. Sits in the
    /// padding byte after bindless_images, so the profile keeps its size (vk_pipeline_serialization.cpp).
    bool heap_images{};
    /// sotc: SOTC_SHADER_ADDR mode (SotcArch::ShaderAddr::Mode): 0 off, 1 guest buffers that are neither formatted
    /// nor coherent take their base and size from their V# words in the flat buffer, and their window bases from
    /// the push block (ShaderAddrBuffer in runtime_info.h), 2 verify: as 1, but each such buffer keeps its
    /// descriptor for the value, and its loads compare the shader address with the descriptor (debug buffer).
    /// 3 and 4 (SOTC_ADDR_TABLE, addr_table.h): as 1 and 2, but each access is translated through the guest page
    /// table (the packet's mirror words are the table window) instead of the window words.
    /// Last member: the cache check pads older profiles. Still one byte (u8 for bool), so sizeof(Profile) holds.
    u8 shader_addr{};

    bool operator==(const Profile&) const = default;
};

} // namespace Shader
