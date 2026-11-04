// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_TEX (ARCH-GCN-NATIVE S3, decisions D7-D9). Which bindings the GPU T#/S# table can express. A binding
// the table key cannot express keeps the heap index-table load (the classic path); the host reads the same predicates
// to decide whether TableMemo::Resolve can be skipped for a stage (TableStage). Pure header on top of resource.h.
#pragma once

#include "shader_recompiler/info.h"
#include "shader_recompiler/resource.h"

namespace Shader::Gcn {

/// An image binding whose view is the T# plus the view class alone: sampled (not written: storage images keep the index
/// table, the device enables non-uniform indexing for sampled arrays only), no 128-bit format, no atomics, no descriptor
/// array, no mip fallback, no constant mip index and no post op (ConvertCubeTo2DArray is applied on top of the T#).
inline bool TableImage(const ImageResource& desc) {
    return !desc.is_written && !desc.is_r128 && !desc.is_atomic && desc.array_size == 0 &&
           desc.mip_fallback_mode == MipStorageFallbackMode::None && desc.constant_mip_index == 0 &&
           desc.post_op == SharpFetchPostOp::None;
}

/// A sampler binding: its S# words and class carry everything GetSharp applies (the post op and its run-time fact).
inline bool TableSampler(const SamplerResource&) {
    return true;
}

/// The stage can read its sharps at run time: the probe loads the T#/S# words from the flat buffer, which exists only
/// when the stage reads user data as data too (shader_info_collection_pass.cpp AddFlatbuf). Without it every binding
/// keeps the index table (found by the offline test: EmitFlatbufferLoad asserted).
inline bool TableReadable(const Info& info) {
    return info.has_flatbuf;
}

/// Every image binding of the stage is expressible by the table (the host may skip the per-op resolve for the stage).
inline bool TableStage(const Info& info) {
    if (!TableReadable(info)) {
        return false;
    }
    for (const ImageResource& image : info.images) {
        if (!TableImage(image)) {
            return false;
        }
    }
    return true;
}

} // namespace Shader::Gcn
