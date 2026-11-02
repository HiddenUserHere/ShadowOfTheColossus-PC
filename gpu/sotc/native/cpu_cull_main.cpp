// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU evaluation of the main-scene XPR cull (BPXPR_BuildSetDraws 0x19b760, dispatch 0x19c0f1, shader
// docs/re-stutters/native/data/cull/2e0f474b.glsl) without the Hi-Z occlusion test. Every object that passes the other
// tests is treated as visible, so the CPU masks are a superset of the GPU masks (see cpu_cull.h).
//
// Bindings of the shader and their guest sources (evidence: the GLSL, and the descriptor staging of 0x19b760: the 0x160
// byte block is memcpy'd to a buffer at [rsp+0x178] (binding 0); the five V# staged from owner+0x30 / +0x58 / +0xd0 /
// +0x80 / +0xa8 at [rsp+0x3c0..0x400] by sub_861ce0; the four mask buffers [rsp+0x300..0x378] written by sub_861e60):
//   binding 0 (ssbo_1) cull block, the 0x160-byte copy of [rsp+0x1a0]. C[0] = n, C[1] = mode, C[2] = plane count,
//                      C[3] = fade field, C[4..67] = planes, C[70] = class mask (+0x118).
//   binding 1 (ssbo_2) object records, 16 dwords (64 bytes) each, owner+0x80 buffer view (stride 0x40, count u16 at
//                      set+0x64). The base is *(set+0x30), where set = *(owner+0x18) (0x19aad0 creates the view with
//                      r9 = [set+0x30], ecx = [set+0x64], edx = 0x40 at 0x19ae2e).
//   binding 2 (ssbo_3) per-model table, 8 floats (32 bytes) per model, owner+0x58 (base [set+0x18]). Written by 0x19b400.
//   binding 3 (ssbo_4) per-model table, 2 dwords (8 bytes) per model, owner+0xd0 (base [set+0x28]).
//   binding 4..7 (ssbo_5..8) mask 0..3, ceil(n / 32) dwords each. Binding 8 (srt_flatbuf) is the dispatch user data:
//                      its dwords 30, 34, 38, 42 are the write guards of the four masks (not reproduced; all dwords of
//                      each mask are written here).
//
// Per object i < n (the shader's thread i; the workgroup ballot packs lane i into bit i % 32 of dword i / 32):
//   gate  : (C[1] & R[14]) != 0 and (C[70] & R[15]) != 0.
//   frust : the segment P = t - c0 .. Q = t + c0 (t = R[3,7,11], c0 = R[0,4,8]) is fattened by |n.c1| + |n.c2| per plane
//           (c1 = R[1,5,9], c2 = R[2,6,10]) and clipped plane by plane. The object is rejected when the clip empties.
//   lod   : model = R[12], lod = R[13]. F6 = T[6], D0a = TD[0], D0b = TD[1]. lodOk = D0a == lod || (F6 < 1 && D0b == lod),
//           comparing float bit patterns with float(lod).
//   fade  : fade = T[C[3]]. The object needs fade > 0.01.
//   hiz   : skipped, treated as visible (the GPU ANDs the Hi-Z result into every mask bit).
//   m0 = visible. m1 = fade > 0.99 && F6 >= 1. m2 = (fade <= 0.99 && F6 >= 1) || (D0b != lod && F6 < 1).
//   m3 = D0b == lod && F6 < 1. Comparisons keep the shader's operand order, so a NaN fails the same tests.
//
// Output is written only when the function returns true; on false the masks are unspecified.
#pragma clang fp contract(off)

#include "cpu_cull.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>

#include "../guest_mem.h"

namespace SotcCpuCull {
namespace {

// The 0x160-byte cull block (cpu_cull.h, BPXPR_BuildSetDraws 0x19b760 at [rsp+0x1a0]).
struct CullBlock {
    uint32_t count;            // 0x00 n, equal to the set's object count
    uint32_t mode;             // 0x04 tested against record[14]
    uint32_t planes;           // 0x08 plane count, at most 16
    uint32_t fade_field;       // 0x0c index into the model's 8-float table (0 in every set seen)
    float plane[16][4];        // 0x10 stored planes (sign-flipped by 0x19b760): a, b, c, d; the shader reads them as is
    float target[2];           // 0x110 Hi-Z target size, unused here
    uint32_t cls;              // 0x118 tested against record[15]
    uint32_t pad;              // 0x11c
    float view_projection[16]; // 0x120 Hi-Z matrix, unused here
};
static_assert(sizeof(CullBlock) == 0x160, "the cull block is 0x160 bytes");

constexpr uint64_t kSetOffset = 0x18;        // owner+0x18: the set record
constexpr uint64_t kSetRecordsOffset = 0x30; // set+0x30: the object records (64 bytes each)
constexpr uint64_t kSetCountOffset = 0x64;   // set+0x64: u16 object count
constexpr uint32_t kRecordBytes = 64;
constexpr uint32_t kMaxPlanes = 16;
constexpr uint32_t kMaxFadeField = 8;        // the table of one model has 8 floats

// clamp(x, 0, 1) as the shader writes it: NaN becomes 0 at the lower bound.
inline float Clamp01(float x) {
    const float lo = std::isnan(x) ? 0.0f : std::max(x, 0.0f);
    return std::min(lo, 1.0f);
}

// The shader's segment clip (2e0f474b.glsl lines 278-378). The segment runs from P = t - c0 to Q = t + c0. Both
// ends carry homogeneous w = 1.0 through the whole loop (the planes are affine), so the w terms reduce to d. Each
// plane is widened by the box support |n.c1| + |n.c2| before the clip. Returns false when the segment leaves the
// planes; the shader then clears its alive flag, and every mask bit of the object is 0.
bool ClipSegment(const float* f, const float* planes, uint32_t nplanes) {
    const float c0x = f[0], c0y = f[4], c0z = f[8];
    const float c1x = f[1], c1y = f[5], c1z = f[9];
    const float c2x = f[2], c2y = f[6], c2z = f[10];
    const float tx = f[3], ty = f[7], tz = f[11];

    float px = tx - c0x, py = ty - c0y, pz = tz - c0z; // P
    float qx = c0x + tx, qy = c0y + ty, qz = c0z + tz; // Q

    for (uint32_t k = 0; k < nplanes; ++k) {
        const float a = planes[4 * k + 0];
        const float b = planes[4 * k + 1];
        const float c = planes[4 * k + 2];
        const float d = planes[4 * k + 3];

        // Support of the box cross-section (c1, c2) in the direction of the plane normal.
        const float dot1 = (c * c1z) + ((b * c1y) + (a * c1x));
        const float dot2 = (c * c2z) + ((b * c2y) + (a * c2x));
        const float fat = std::fabs(dot1) + std::fabs(dot2);

        // Plane values at Q and P (the d term is d * w with w = 1).
        const float q_val = d + ((c * qz) + ((b * qy) + ((a * qx) + fat)));
        const float p_val = d + ((c * pz) + ((b * py) + ((a * px) + fat)));
        const bool q_in = 0.0f <= q_val;
        const bool p_in = 0.0f <= p_val;
        if (!q_in && !p_in) {
            return false; // both ends outside a half-space: the segment is outside it
        }

        // n.(Q - P); the shader adds d * (w_Q - w_P) = d * 0 here.
        const float dx = qx - px, dy = qy - py, dz = qz - pz;
        const float den = (d * (1.0f - 1.0f)) + ((c * dz) + ((b * dy) + (a * dx)));

        const bool crossing = q_in != p_in;
        const float t = crossing ? Clamp01((1.0f / den) * q_val) : 0.0f;
        const bool keep_q = crossing && (q_val >= p_val);

        // The new P is the crossing point (for a segment without a crossing it is the old Q). Q keeps its inside end:
        // when Q is outside, it takes the old P, so the two ends swap.
        const float ix = (px - qx) * t + qx;
        const float iy = (py - qy) * t + qy;
        const float iz = (pz - qz) * t + qz;
        if (keep_q) {
            px = ix;
            py = iy;
            pz = iz;
        } else {
            const float ox = px, oy = py, oz = pz;
            px = ix;
            py = iy;
            pz = iz;
            qx = ox;
            qy = oy;
            qz = oz;
        }
    }
    return true;
}

} // namespace

bool CullMain(uint64_t owner, uint64_t cull_block, uint32_t count, uint32_t* const masks[4]) {
    if (owner == 0 || cull_block == 0 || count == 0 || count > 0xffffu || masks[0] == nullptr ||
        masks[1] == nullptr || masks[2] == nullptr || masks[3] == nullptr) {
        return false;
    }

    CullBlock cb{};
    if (!SotcGuest::Read(cull_block, &cb, sizeof cb)) {
        return false;
    }
    if (cb.count != count || cb.planes > kMaxPlanes || cb.fade_field >= kMaxFadeField) {
        return false;
    }
    for (uint32_t k = 0; k < cb.planes; ++k) {
        for (uint32_t j = 0; j < 4; ++j) {
            if (!std::isfinite(cb.plane[k][j])) {
                return false;
            }
        }
    }

    // The records of the set: the same base and count the GPU view at owner+0x80 is built from.
    uint64_t set = 0;
    uint16_t set_count = 0;
    uint64_t records = 0;
    if (!SotcGuest::Read(owner + kSetOffset, &set) || set == 0 ||
        !SotcGuest::Read(set + kSetCountOffset, &set_count) || set_count != count ||
        !SotcGuest::Read(set + kSetRecordsOffset, &records) || records == 0) {
        return false;
    }
    const uint8_t* rec_base = SotcGuest::Backing(records, uint64_t{count} * kRecordBytes);
    if (rec_base == nullptr) {
        return false;
    }

    std::optional<OwnerLod> mirror; // the LOD mirror of the owner, pinned on first use (a set the frustum empties never needs it)
    const uint32_t words = (count + 31) / 32;
    for (uint32_t w = 0; w < words; ++w) {
        uint32_t bits[4] = {0, 0, 0, 0};
        const uint32_t first = w * 32;
        const uint32_t last = std::min(first + 32, count);
        for (uint32_t i = first; i < last; ++i) {
            uint32_t r[16];
            std::memcpy(r, rec_base + uint64_t{i} * kRecordBytes, kRecordBytes);

            // Mode and class gates (the shader's _214 and _227).
            if ((cb.mode & r[14]) == 0 || (cb.cls & r[15]) == 0) {
                continue;
            }

            // Oriented box: the segment and the two fattening axes must be finite for the clip to mean anything.
            float f[12];
            for (uint32_t j = 0; j < 12; ++j) {
                f[j] = std::bit_cast<float>(r[j]);
                if (!std::isfinite(f[j])) {
                    return false;
                }
            }
            if (!ClipSegment(f, &cb.plane[0][0], cb.planes)) {
                continue;
            }

            if (!mirror) {
                mirror.emplace(owner, cb.fade_field);
                if (!mirror->ok()) {
                    return false;
                }
            }
            ModelBits ms;
            if (!mirror->Get(r[12], &ms)) {
                return false;
            }

            // LOD match (the shader's _265): the current LOD, or the fade target while F6 < 1.
            const float lod = static_cast<float>(r[13]);
            const float f6 = ms.p;
            const float fade = ms.fade;
            const bool d0a = std::bit_cast<float>(ms.d0[0]) == lod;
            const bool d0b = std::bit_cast<float>(ms.d0[1]) == lod;
            const bool f6_below_one = 1.0f > f6; // the shader's _1151
            if (!(d0a || (f6_below_one && d0b))) {
                continue;
            }
            // Fade gate (the shader's _490).
            if (!(0.01f < fade)) {
                continue;
            }

            const bool faded_in = 0.99f < fade; // the shader's _1159
            const uint32_t bit = uint32_t{1} << (i - first);
            bits[0] |= bit; // visible (Hi-Z assumed visible)
            if (!f6_below_one && faded_in) {
                bits[1] |= bit;
            }
            if ((!f6_below_one && !faded_in) || (f6_below_one && !d0b)) {
                bits[2] |= bit;
            }
            if (f6_below_one && d0b) {
                bits[3] |= bit;
            }
        }
        masks[0][w] = bits[0];
        masks[1][w] = bits[1];
        masks[2][w] = bits[2];
        masks[3][w] = bits[3];
    }
    return true;
}

} // namespace SotcCpuCull
