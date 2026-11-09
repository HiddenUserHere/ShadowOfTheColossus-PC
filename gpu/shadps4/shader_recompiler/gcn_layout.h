// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: ARCH-GCN-NATIVE (docs/re-stutters/native/ARCH-GCN-NATIVE.md, decisions in GCN-NATIVE-DECISIONS.md). The
// GPU-visible layouts shared by the recompiler (which emits code that reads them) and the host (which fills them).
// Pure header: no Vulkan, no video_core. Change it only together with every reader and writer, and bump the shader
// cache version (vk_pipeline_serialization.cpp ShaderBinaryVersion) when a layout below changes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Shader::Gcn {

// ---------------------------------------------------------------------------------------------------------------
// The globals buffer. One host-visible, host-coherent, persistently mapped buffer with a device address, created
// once (SotcGcn::Globals::Init). Its device address reaches the shaders in the push packet: DrawPacket::
// arena_guest_base (push byte offset ShaderAddrArenaGuestOffset = 88) carries it whenever the pipeline's profile has
// gcn_buf or gcn_tex set (those pipelines never use the arena window words: the arena is in the page table, S1).
//
//   [0, 64)                       GlobalsHeader
//   [miss_ring_offset, ...)       MissRing: header (16 bytes) + kMissRingCapacity MissRecord
//   [seen_offset, ...)            seen bitmap: kSeenBits bits (u32 words), miss dedup
//   [tex_table_offset, ...)       TexEntry[kTexTableCapacity] (S3)
struct GlobalsHeader {
    uint64_t page_table_address; ///< SotcArch::AddrTable::Base() (0: no table)
    uint64_t page_table_guest_base; ///< AddrTable::WindowBase
    uint64_t page_table_size; ///< AddrTable::WindowSize (bytes of guest window)
    uint32_t miss_ring_offset; ///< byte offset of the MissRing from the globals base
    uint32_t seen_offset; ///< byte offset of the seen bitmap
    uint32_t tex_table_offset; ///< byte offset of TexEntry[0]
    uint32_t tex_table_mask; ///< kTexTableCapacity - 1
    uint32_t null_image_slot; ///< reserved (the null slot is per key: NullSlotOf in the shader is slot 0 of the key)
    uint32_t flags; ///< bit 0: misses are recorded (gcn_buf or gcn_tex on)
    uint32_t reserved[4];
};
static_assert(sizeof(GlobalsHeader) == 64);

// ---------------------------------------------------------------------------------------------------------------
// Miss feedback (S2 buffers, S3 textures). A shader that finds no answer appends one record; the host drains the ring
// once per submit (SotcGcn::Globals::DrainMisses) after the fence of the work that wrote it. Dedup: before appending,
// the shader sets bit (hash & (kSeenBits-1)) of the seen bitmap with an atomic OR and appends only when the bit was
// clear. The host clears the whole bitmap after each drain.
//
// Append protocol (shader): idx = atomicAdd(ring.write_index, 1); the slot is idx & (capacity - 1); the shader
// writes the record body, then stores stamp = idx + 1 last. The host reads slots from its
// read index while stamp == read_index + 1, and counts an overflow when write_index - read_index > capacity.
inline constexpr uint32_t kMissRingCapacity = 4096; ///< power of two
inline constexpr uint32_t kSeenBits = 1u << 16;
struct MissRingHeader {
    uint32_t write_index; ///< atomically incremented by shaders (wraps at 2^32; the host compares modulo)
    uint32_t reserved[3];
};
static_assert(sizeof(MissRingHeader) == 16);
enum MissKind : uint32_t {
    kMissNone = 0,
    kMissPage = 1,    ///< S2: a guest page whose table entry was 0. words[0..1] = guest address (lo, hi)
    kMissImage = 2,   ///< S3: a T# not in the table. words[0..7] = T#, view_class = the binding's ViewClass
    kMissSampler = 3, ///< S3: an S# not in the table. words[0..3] = S#, view_class = its SamplerClass
};
// sotc: kMissPage payload beyond the page: view_class = flags below, words[2..3] = the access's V# guest base (lo, hi),
// words[4] = its block size in bytes (the V# size). The host makes the whole binding resident for a formatted or
// written access (as its bind does) and only the page for a plain read.
inline constexpr uint32_t kMissPageFormatted = 1u; ///< the buffer is a formatted (texel) one
inline constexpr uint32_t kMissPageWritten = 2u;   ///< the buffer is written by the shader
// sotc: the stamp is stored with an atomic store of Release semantics (Device scope, UniformMemory) after the body, so
// a host that sees the stamp also sees the body (the host reads the stamp, an acquire fence, then the body).
struct MissRecord {
    uint32_t stamp; ///< ring index + 1, written last
    uint32_t kind; ///< MissKind
    uint32_t view_class; ///< kMissImage: ViewClass; kMissSampler: SamplerClass; otherwise 0
    uint32_t reserved;
    uint32_t words[8];
};
static_assert(sizeof(MissRecord) == 48);

// ---------------------------------------------------------------------------------------------------------------
// The T# table (S3). Open addressing, linear probing, at most kTexMaxProbes probes from hash & mask. The shader
// compares all key words; the host never moves a live entry (it tombstones, then reuses a tombstone only on insert).
inline constexpr uint32_t kTexTableCapacity = 1u << 16; ///< power of two; 64 Ki entries * 48 bytes = 3 MiB
inline constexpr uint32_t kTexMaxProbes = 16;
enum TexState : uint32_t { kTexEmpty = 0, kTexLive = 1, kTexTombstone = 2 };
/// Sampler classes have the top byte 0xFF (images never do: their class has bits 8.. = the bindless key, below 2^16).
/// The class carries what SamplerResource::GetSharp applies on top of the raw S# words, so (S# words, class) fixes the
/// sampler: is_depth (bit 0), the post op (bits 1..6, Shader::SharpFetchPostOp), and for DisableAnisoIfSingleLod the
/// run-time fact the post op reads from the paired T# (bit 7: ((tsharp dw3 >> 12) & 0xff) == 0). The shader computes
/// bit 7 at run time from the T# dword; the host rebuilds the sampler from the class without reading any T#.
constexpr uint32_t SamplerClass(bool is_depth, uint32_t post_op, bool single_lod) {
    return 0xFF000000u | (is_depth ? 1u : 0u) | ((post_op & 0x3Fu) << 1) | (single_lod ? 0x80u : 0u);
}
constexpr bool IsSamplerClass(uint32_t view_class) {
    return (view_class >> 24) == 0xFFu;
}
struct TexEntry {
    uint32_t words[8]; ///< the T# (8 dwords) or the S# (words 0..3, words 4..7 zero)
    uint32_t view_class; ///< ViewClass of the binding, SamplerClass for samplers
    uint32_t slot; ///< heap slot index in the array of the image's bindless key (or the sampler heap)
    uint32_t state; ///< TexState; written last by the host (after words, class and slot)
    uint32_t reserved;
};
static_assert(sizeof(TexEntry) == 48);

/// The view class of an image binding: the bits ImageViewInfo depends on besides the T# (image_view.cpp:76) and the
/// bindless array key (Shader::BindlessImageKeyOf, below 2^24). Computed at compile time by the recompiler (a constant
/// per binding) and by the host when it inserts. Two bindings with the same T# and class share one view.
constexpr uint32_t ViewClass(bool is_written, bool is_depth, bool is_array, uint32_t bindless_key) {
    return (is_written ? 1u : 0u) | (is_depth ? 2u : 0u) | (is_array ? 4u : 0u) | ((bindless_key & 0xFFFFu) << 8);
}

// ---------------------------------------------------------------------------------------------------------------
// The hash, the same on the host and in SPIR-V (32-bit integer ops only: multiply, xor, shifts). murmur3_32 over n
// words with a seed. Key of a T# entry: Hash(words, 8, view_class). Of an S# entry: Hash(words, 4, sampler_class).
// Seen bit of a page miss: Hash({lo, hi}, 2, kMissPage) where (lo, hi) is the page-aligned guest address.
constexpr uint32_t Rotl32(uint32_t x, uint32_t r) {
    return r == 0 ? x : (x << r) | (x >> (32u - r)); // r == 0 would shift by 32 (undefined); callers use 13 and 15
}
constexpr uint32_t Hash(const uint32_t* words, uint32_t n, uint32_t seed) {
    uint32_t h = seed ^ 0x9E3779B9u;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t k = words[i] * 0xCC9E2D51u;
        k = Rotl32(k, 15) * 0x1B873593u;
        h ^= k;
        h = Rotl32(h, 13) * 5u + 0xE6546B64u;
    }
    h ^= n * 4u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

/// Byte layout of the globals buffer (offsets from its base). The host creates it with exactly GlobalsSize bytes.
inline constexpr uint32_t kMissRingOffset = 64;
inline constexpr uint32_t kSeenOffset = kMissRingOffset + 16 + kMissRingCapacity * 48;
inline constexpr uint32_t kTexTableOffset = kSeenOffset + kSeenBits / 8;
inline constexpr uint32_t kGlobalsSize = kTexTableOffset + kTexTableCapacity * 48;
static_assert(kTexTableOffset % 16 == 0);

} // namespace Shader::Gcn
