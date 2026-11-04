// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_CENSUS=1 (off by default; measurement only, no behaviour change). Stage S0 of
// docs/re-stutters/native/ARCH-GCN-NATIVE.md: what the draw thread's ops bind, to size the stages that move resource
// resolution into the shaders (buffers by address, textures by a T# table). Per op (Draw, DrawIndirect,
// DispatchDirect, DispatchIndirect), from the pipeline's stages and the op's sharps:
//   - the op kind, and whether its shaders write memory, use storage images, fmasks or DMA reads;
//   - buffers: plain read guest buffers, formatted (texel) ones, written ones, the port's special buffers; how many of
//     the guest ones are served from the imported arena (SOTC_PC_RING=8); their bound bytes, in size buckets;
//   - images: sampled and storage, arrays, 3D, cubes, mip tails (base level above 0), descriptor arrays;
//   - the distinct T#s and S#s of each frame, and the T#s never seen before in the session (first uses).
// Every 5 s the draw thread prints three "SOTC GCN CENSUS" lines.
#pragma once

#include <cstdint>

namespace SotcGcnCensus {

/// SOTC_GCN_CENSUS=1, read once.
bool On() noexcept;

enum Kind : uint8_t { kDraw, kDrawIndirect, kDispatch, kDispatchIndirect, kKinds };
constexpr uint32_t kSizeBuckets = 5; ///< < 4 KiB, < 64 KiB, < 1 MiB, < 16 MiB, larger

/// One op's resources (filled by Rasterizer::CensusNote).
struct Op {
    uint8_t kind = kDraw;
    bool writes_memory = false, storage = false, fmask = false, dma = false;
    uint32_t buf_plain = 0, buf_formatted = 0, buf_written = 0, buf_special = 0, buf_host = 0;
    uint32_t img_sampled = 0, img_storage = 0, img_array = 0, img_3d = 0, img_cube = 0, img_mip = 0, img_desc_array = 0;
    uint32_t samplers = 0;
    uint64_t buf_bytes = 0;
    uint32_t size_hist[kSizeBuckets] = {};
};

/// Draw thread: one op. tsharps / ssharps: hashes of the op's T# and S# words. frame: the presented frame count.
void Note(const Op& op, const uint64_t* tsharps, uint32_t tsharp_count, const uint64_t* ssharps,
          uint32_t ssharp_count, uint64_t frame) noexcept;

} // namespace SotcGcnCensus
