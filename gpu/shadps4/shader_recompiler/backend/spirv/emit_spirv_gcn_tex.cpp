// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_TEX (ARCH-GCN-NATIVE S3, decisions D7-D9 in docs/re-stutters/native/GCN-NATIVE-DECISIONS.md). A heap
// stage with Profile::gcn_tex == 1 takes the heap index of each image and sampler from the GPU T#/S# table in the
// globals buffer (gcn_layout.h), not from the draw's index table. The probe runs once per invocation, in the entry
// block (HoistGcnTex, from HoistBdaBlocks). Its control flow depends only on the sharps, which are uniform across a
// draw, so every invocation takes the same path.
#include <array>
#include <cstddef>
#include <span>

#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/gcn_layout.h"
#include "shader_recompiler/gcn_rules.h"

namespace Shader::Backend::SPIRV {

namespace {

constexpr u32 kEntryWordsOffset = 0;
constexpr u32 kEntryClassOffset = static_cast<u32>(offsetof(Shader::Gcn::TexEntry, view_class));
constexpr u32 kEntrySlotOffset = static_cast<u32>(offsetof(Shader::Gcn::TexEntry, slot));
constexpr u32 kEntryStateOffset = static_cast<u32>(offsetof(Shader::Gcn::TexEntry, state));
constexpr u32 kEntrySize = static_cast<u32>(sizeof(Shader::Gcn::TexEntry));
constexpr u32 kMissRecordSize = static_cast<u32>(sizeof(Shader::Gcn::MissRecord));
/// Bit 7 of a sampler class: the run-time DisableAnisoIfSingleLod fact (gcn_layout.h SamplerClass).
constexpr u32 kSingleLodBit = 0x80u;

/// One dword of a sharp, as SharpFetch::Fetch reads it (the rule of LoadTsharpDword in emit_spirv_image.cpp).
template <class T>
Id SharpDword(EmitContext& ctx, const SharpFetch<T>& fetch, u32 dw) {
    if (fetch.summary == SharpFetch<T>::Summary::SingleLoad) {
        return ctx.EmitFlatbufferLoad(ctx.ConstU32(u32(fetch.offsets[0]) + dw));
    }
    if (fetch.load_mask & (1u << dw)) {
        return ctx.EmitFlatbufferLoad(ctx.ConstU32(u32(fetch.offsets[dw])));
    }
    return ctx.ConstU32(fetch.immediates[dw]);
}

} // namespace

void EmitContext::HoistGcnTex() {
    // sotc: SOTC_GCN_TEX. A stage with no flat buffer cannot load its sharps at run time: it keeps the index table for
    // every binding (gcn_rules.h TableReadable), exactly as gcn_tex == 0.
    if (!uses_heap_images || profile.gcn_tex != 1 || !Shader::Gcn::TableReadable(info)) {
        return;
    }
    // The variables first: OpVariable must lead the entry block, and HoistBdaBlocks calls this before any load.
    const Id pointer_type{TypePointer(spv::StorageClass::Function, U32[1])};
    gcn_probe_var = AddLocalVariable(pointer_type, spv::StorageClass::Function);
    gcn_state_var = AddLocalVariable(pointer_type, spv::StorageClass::Function);
    gcn_result_var = AddLocalVariable(pointer_type, spv::StorageClass::Function);

    // Invalid sharps keep the null slot 0 (no probe, no miss: the sharp names no resource). A binding the table key
    // cannot express (TableImage) has no slot here: its id stays invalid and LoadHeapImageIndex keeps the index table.
    gcn_image_slots.assign(info.images.size(), Id{});
    gcn_image_probed.assign(info.images.size(), false);
    for (u32 i = 0; i < info.images.size(); ++i) {
        const ImageResource& desc = info.images[i];
        if (desc.sharp_fetch.summary == SharpFetch<AmdGpu::Image>::Summary::Invalid) {
            gcn_image_slots[i] = u32_zero_value;
            continue;
        }
        if (!Shader::Gcn::TableImage(desc)) {
            continue;
        }
        std::array<Id, 8> key{};
        for (u32 dw = 0; dw < key.size(); ++dw) {
            key[dw] = SharpDword(*this, desc.sharp_fetch, dw);
        }
        const u32 view_class{Shader::Gcn::ViewClass(desc.is_written, desc.is_depth, desc.is_array,
                                                    BindlessImageKeyOf(info, desc))};
        gcn_image_slots[i] = GcnTexLookup(key, ConstU32(view_class), false);
        gcn_image_probed[i] = true;
        // sotc: SOTC_GCN_TEX. The slot differs per invocation when the host tombstones it: non-uniform indexing.
        AddCapability(spv::Capability::ShaderNonUniform);
        AddCapability(spv::Capability::SampledImageArrayNonUniformIndexing);
    }

    gcn_sampler_slots.assign(info.samplers.size(), u32_zero_value);
    gcn_sampler_probed.assign(info.samplers.size(), false);
    for (u32 s = 0; s < info.samplers.size(); ++s) {
        const SamplerResource& samp = info.samplers[s];
        if (samp.sharp_fetch.summary == SharpFetch<AmdGpu::Sampler>::Summary::Invalid) {
            continue;
        }
        std::array<Id, 4> key{};
        for (u32 dw = 0; dw < key.size(); ++dw) {
            key[dw] = SharpDword(*this, samp.sharp_fetch, dw);
        }
        // The class: SamplerClass of the constants, plus bit 7 at run time for DisableAnisoIfSingleLod (the same test
        // as SamplerResource::GetSharp: the paired T#'s dword 3, bits 12..19, is zero).
        Id view_class{ConstU32(Shader::Gcn::SamplerClass(samp.is_depth, u32(samp.post_op), false))};
        if (samp.post_op == Shader::SharpFetchPostOp::DisableAnisoIfSingleLod) {
            const Id tsharp_dw3{EmitFlatbufferLoad(ConstU32(u32(samp.post_op_tsharp_dw3_off)))};
            const Id single_lod{OpIEqual(U1[1], OpBitFieldUExtract(U32[1], tsharp_dw3, ConstU32(12u), ConstU32(8u)),
                                         u32_zero_value)};
            view_class = OpBitwiseOr(U32[1], view_class,
                                     OpSelect(U32[1], single_lod, ConstU32(kSingleLodBit), u32_zero_value));
        }
        gcn_sampler_slots[s] = GcnTexLookup(key, view_class, true);
        gcn_sampler_probed[s] = true;
        // sotc: SOTC_GCN_TEX. Sampler arrays are indexed non-uniformly as well (there is no separate sampler capability in
        // SPIR-V 1.6: the Vulkan feature shaderSampledImageArrayNonUniformIndexing covers them).
        AddCapability(spv::Capability::ShaderNonUniform);
        AddCapability(spv::Capability::SampledImageArrayNonUniformIndexing);
    }
}

Id EmitContext::GcnGlobalsBase() {
    return OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64), push_data_block,
                                     ConstU32(HeapGcnGlobalsMember)));
}

Id EmitContext::GcnHashWithSeed(std::span<const Id> words, Id seed) {
    // The operations of Shader::Gcn::Hash (gcn_layout.h), in the same order, with the seed as a run-time value.
    const auto rotl = [&](Id x, u32 r) {
        return OpBitwiseOr(U32[1], OpShiftLeftLogical(U32[1], x, ConstU32(r)),
                           OpShiftRightLogical(U32[1], x, ConstU32(32u - r)));
    };
    Id h{OpBitwiseXor(U32[1], seed, ConstU32(0x9E3779B9u))};
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

Id EmitContext::GcnTexLookup(std::span<const Id> key, Id view_class, bool sampler) {
    const u32 n = static_cast<u32>(key.size());
    const Id globals{GcnGlobalsBase()};
    const Id hash{GcnHashWithSeed(key, view_class)};

    // Linear probing as a loop: the probe number, the outcome and the slot live in function variables.
    OpStore(gcn_probe_var, u32_zero_value);
    OpStore(gcn_state_var, u32_zero_value);
    OpStore(gcn_result_var, u32_zero_value);
    const Id header{OpLabel()};
    const Id body{OpLabel()};
    const Id cont{OpLabel()};
    const Id merge{OpLabel()};
    OpBranch(header);

    // The header holds the loop test: OpLoopMerge is directly followed by the conditional branch that exits the loop.
    AddLabel(header);
    const Id probe{OpLoad(U32[1], gcn_probe_var)};
    const Id state{OpLoad(U32[1], gcn_state_var)};
    const Id more{OpLogicalAnd(U1[1], OpULessThan(U1[1], probe, ConstU32(Shader::Gcn::kTexMaxProbes)),
                               OpIEqual(U1[1], state, u32_zero_value))};
    OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
    OpBranchConditional(more, body, merge);

    AddLabel(body);
    const Id index{OpBitwiseAnd(U32[1], OpIAdd(U32[1], hash, probe),
                                ConstU32(Shader::Gcn::kTexTableCapacity - 1))};
    const Id entry_offset{OpIAdd(U64, OpIMul(U64, OpUConvert(U64, index), Constant(U64, u64{kEntrySize})),
                                 Constant(U64, u64{Shader::Gcn::kTexTableOffset}))};
    const Id entry{OpIAdd(U64, globals, entry_offset)};
    const auto field = [&](u32 offset) {
        return OpLoad(U32[1], BdaPointer(OpIAdd(U64, entry, Constant(U64, u64{offset})), U32[1]),
                      spv::MemoryAccessMask::Aligned, 4U);
    };
    // Live first: a tombstone keeps its key words, so it must not match. Then the class, then the words.
    const Id entry_state{field(kEntryStateOffset)};
    const Id is_live{OpIEqual(U1[1], entry_state, ConstU32(u32(Shader::Gcn::kTexLive)))};
    const Id is_empty{OpIEqual(U1[1], entry_state, ConstU32(u32(Shader::Gcn::kTexEmpty)))};
    Id same{OpIEqual(U1[1], field(kEntryClassOffset), view_class)};
    for (u32 w = 0; w < n; ++w) {
        same = OpLogicalAnd(U1[1], same, OpIEqual(U1[1], field(kEntryWordsOffset + 4 * w), key[w]));
    }
    const Id hit{OpLogicalAnd(U1[1], is_live, same)};
    const Id slot{field(kEntrySlotOffset)};
    // A miss leaves 0 in the result: the last iteration writes 0 unless it hit.
    OpStore(gcn_result_var, OpSelect(U32[1], hit, slot, u32_zero_value));
    OpStore(gcn_state_var, OpSelect(U32[1], hit, ConstU32(1u),
                                    OpSelect(U32[1], is_empty, ConstU32(2u), u32_zero_value)));
    OpBranch(cont);

    AddLabel(cont);
    OpStore(gcn_probe_var, OpIAdd(U32[1], OpLoad(U32[1], gcn_probe_var), ConstU32(1u)));
    OpBranch(header);

    AddLabel(merge);
    const Id final_state{OpLoad(U32[1], gcn_state_var)};
    const Id result{OpLoad(U32[1], gcn_result_var)};
    const Id missed{OpINotEqual(U1[1], final_state, ConstU32(1u))};
    const Id miss_merge{BdaBeginIf(missed)};
    GcnTexRecordMiss(globals, sampler ? u32(Shader::Gcn::kMissSampler) : u32(Shader::Gcn::kMissImage), key,
                     view_class, hash);
    BdaEndIf(miss_merge);
    return result;
}

void EmitContext::GcnTexRecordMiss(Id globals, u32 kind, std::span<const Id> key, Id view_class, Id hash) {
    // Same protocol as GcnPageMiss (emit_spirv_context.cpp): seen bit first, then the ring index, the body, and the
    // stamp last (gcn_layout.h). The record is dropped (the slot wraps) when the ring is full; the host counts it.
    const Id scope{ConstU32(static_cast<u32>(spv::Scope::Device))};
    const auto field = [&](u32 offset) {
        return BdaPointer(OpIAdd(U64, globals, Constant(U64, u64{offset})), U32[1]);
    };
    const Id bit{OpBitwiseAnd(U32[1], hash, ConstU32(Shader::Gcn::kSeenBits - 1))};
    const Id seen_offset{OpIAdd(U32[1],
                                OpShiftLeftLogical(U32[1], OpShiftRightLogical(U32[1], bit, ConstU32(5u)),
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
    const Id record_offset{OpIAdd(U32[1], ConstU32(Shader::Gcn::kMissRingOffset + 16),
                                  OpIMul(U32[1], slot, ConstU32(kMissRecordSize)))};
    const Id record{OpIAdd(U64, globals, OpUConvert(U64, record_offset))};
    const auto body = [&](u32 offset, Id value) {
        OpStore(BdaPointer(OpIAdd(U64, record, Constant(U64, u64{offset})), U32[1]), value,
                spv::MemoryAccessMask::Aligned, 4u);
    };
    body(4, ConstU32(kind));
    body(8, view_class);
    body(12, u32_zero_value);
    // Words 4..7 of an S# are zero (the sampler key is 4 words), so the slot never keeps a stale key.
    for (u32 w = 0; w < 8; ++w) {
        body(16 + 4 * w, w < key.size() ? key[w] : u32_zero_value);
    }
    GcnStoreStamp(record, OpIAdd(U32[1], idx, ConstU32(1u))); // stamp, last (sotc: atomic store, Release)
    BdaEndIf(fresh_merge);
}

} // namespace Shader::Backend::SPIRV
