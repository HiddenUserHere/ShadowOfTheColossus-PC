// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU evaluation of the shadow visibility masks (sub_19c1a0, dispatch site 0x19c6b1), option B of
// docs/re-stutters/native/CPUCULL-BRIEF.md. It replays the cull shader docs/re-stutters/native/data/cull/4defcf7f.glsl
// (2 masks, no Hi-Z) on the CPU at the producer site, so the consumer gets the current frame's masks.
//
// Bindings of the shader, mapped to the producer's V# (owner = sub_19c1a0 param_1, set = *(owner+0x18)):
//   0  cull block, 0x160 bytes: the producer's stack copy at rsp+0xa0 (= rbp-0x2e0), copied to the GPU before the
//      dispatch. Planes at +0x10 (16 bytes each), mode at +0x04, class at +0x118, fade slot at +0x0c.
//   1  object records, 64 bytes per object: V# owner+0x80, data = set+0x30 (no copy: the 0x800 flag).
//   2  per-model LOD table, 64 bytes per model: V# owner+0xa8, data = set+0x00 (no copy). Dword 1 is the LOD the
//      object's record must match. An input of the GPU pre-shader too; read directly.
//   3  per-model fade table, 32 bytes (8 floats) per model: V# owner+0x58, GPU storage written by the pre-shader
//      0x19b400 each frame. Not in guest memory yet at the producer site: taken from SotcCpuCull::ModelLodState.
//   4  mask A (Shadow bit: fade > 0.99): the buffer at scene slot +0x20 + 8*index, masks[0].
//   5  mask B (ShadowFadeOut bit: 0.01 < fade <= 0.99): the buffer at +0x60 + 8*index, masks[1].
//   6  srt flat buffer (a copy of the user data): dwords 18 and 22 are the mask dword counts (the stores' guards).
// The binding of masks to V# follows the user-data offsets 16 and 20 of the shader's sharp fetches (the same
// convention the HLE path checks in sotc_shadow_cull.cpp); the producer passes {A, B} in slots 1 and 2.
//
// Exactness. The arithmetic replays the shader in single precision in its own operation order (this file is built
// without contraction). A device may differ from that replay in the reciprocal of the clip denominator (2.5 ulp,
// v_rcp_f32) and in denormal flushing. Those can flip a sign the shader takes, and a flipped sign can clear a bit
// the GPU sets, which the conservative rule forbids. So each decision must clear an error bound; when one does not,
// the object is kept (a superset), as in the HLE replay sotc_shadow_cull.cpp (its EvaluateSection / Superset are
// the same math, copied here because that file depends on the Vulkan headers). The fade thresholds are widened by
// kFadeEps for the same reason: ModelLodState's fade is only as exact as the pre-shader replay.
//
// Results are returned in the GPU's layout: bit (index & 31) of dword (index >> 5), one dword per 32 objects.

#include "cpu_cull.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "../guest_mem.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace SotcCpuCull {
namespace {

// Diagnostic counters of the shadow replay (printed with the LOD mirror line, ShadowDebugLine).
struct ShadowDebug {
    std::atomic<uint64_t> calls{0}, rejected_input{0}, objects{0}, gated{0}, lod_reject{0}, geometry_culled{0};
    std::atomic<uint64_t> state_refused{0}, no_bit{0}, bit_a{0}, bit_b{0};
    std::atomic<int> logs{0};
};
ShadowDebug g_dbg;

// Producer layout (sub_19c1a0, see the header comment).
constexpr uint64_t kOwnerSetOffset = 0x18;      // owner+0x18: the set header
constexpr uint64_t kSetLodOffset = 0x00;        // set+0x00: per-model 64-byte LOD table (V# owner+0xa8)
constexpr uint64_t kSetRecordsOffset = 0x30;    // set+0x30: per-object 64-byte records (V# owner+0x80)
constexpr uint64_t kSetModelCountOffset = 0x58; // set+0x58: u16 model count
constexpr uint64_t kSetCountOffset = 0x64;      // set+0x64: u16 object count
constexpr size_t kBlockBytes = 0x160;           // the cull block (binding 0)
constexpr size_t kRecordBytes = 64;
constexpr size_t kLodRecordBytes = 64;
constexpr uint32_t kMaxPlanes = 21;             // planes that fit the block (dwords 4..87), as the HLE replay

// Threshold of the shader's fade tests (the float nearest to 0.99 and 0.01, as the shader's literals).
constexpr float kFadeFull = 0.9900000095367431640625f;
constexpr float kFadeMin = 0.00999999977648258209228515625f;
// Half-width of the band around each threshold in which the CPU fade may disagree with the GPU's fade.
constexpr double kFadeEps = 1e-5;

// Error model of the device, as sotc_shadow_cull.cpp (see its comments for the derivation).
constexpr double kRel = 0x1p-22;  ///< per-operation error of the device (2 ulp at worst)
constexpr double kRcp = 0x1p-20;  ///< reciprocal error of the device (8 ulp at worst)
constexpr double kAbs = 0x1p-100; ///< denormal flushing, per unit of operand magnitude
constexpr double kChain = 24.0;   ///< operations behind one value (19) x (device + our rounding)

float F(uint32_t v) {
    return std::bit_cast<float>(v);
}

uint32_t U(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

double A(float v) {
    return std::fabs(static_cast<double>(v));
}

struct Plane {
    float x, y, z, w;
};

enum class Result { Culled, Visible, Kept };

/// The plane distances of both ends and the clip denominator, in the shader's operation order, with the magnitudes
/// their errors scale with.
struct PlaneEval {
    float dQ, dP, den;
    double lQ, lP;
};

/// The magnitudes the error of the clip denominator scales with (only needed on a straddle).
double DenScale(const Plane& pl, const float* p, const float* q, float wp, float wq) {
    return A(pl.x) * (A(p[0]) + A(q[0])) + A(pl.y) * (A(p[1]) + A(q[1])) + A(pl.z) * (A(p[2]) + A(q[2])) +
           A(pl.w) * (A(wp) + A(wq));
}

PlaneEval EvalPlane(const Plane& pl, const float* ax, const float* bx, const float* p, const float* q, float wp,
                    float wq) {
    PlaneEval e;
    const float t309 = pl.x * ax[0];
    const float t312 = pl.y * ax[1];
    const float t313 = t312 + t309;
    const float t315 = pl.x * bx[0];
    const float t318 = pl.z * ax[2];
    const float t319 = t318 + t313;
    const float t321 = pl.y * bx[1];
    const float t322 = t321 + t315;
    const float t324 = pl.z * bx[2];
    const float t325 = t324 + t322;
    const float r = std::fabs(t319) + std::fabs(t325);
    const float t330 = q[0] - p[0];
    const float t331 = pl.x * t330;
    const float t288 = q[1] - p[1];
    const float t333 = pl.y * t288;
    const float t334 = t333 + t331;
    const float t332 = q[2] - p[2];
    const float t335 = pl.z * t332;
    const float t336 = t335 + t334;
    const float t339 = wq - wp;
    const float t341 = pl.w * t339;
    e.den = t341 + t336;
    const float t343 = pl.x * p[0];
    const float t344 = t343 + r;
    const float t345 = pl.x * q[0];
    const float t346 = t345 + r;
    const float t347 = pl.y * q[1];
    const float t348 = t347 + t346;
    const float t349 = pl.y * p[1];
    const float t350 = t349 + t344;
    const float t351 = pl.z * q[2];
    const float t352 = t351 + t348;
    const float t353 = pl.z * p[2];
    const float t354 = t353 + t350;
    const float t355 = pl.w * wq;
    e.dQ = t355 + t352;
    const float t357 = pl.w * wp;
    e.dP = t357 + t354;
    // Every intermediate of a sum is bounded by the sum of the magnitudes of its terms.
    const double lr = A(t309) + A(t312) + A(t315) + A(t318) + A(t321) + A(t324);
    e.lQ = lr + A(t345) + A(t347) + A(t351) + A(t355);
    e.lP = lr + A(t343) + A(t349) + A(t353) + A(t357);
    return e;
}

double MaxCoord(const float* p, const float* q) {
    double m = 0.0;
    for (int i = 0; i < 3; ++i) {
        m = std::max({m, A(p[i]), A(q[i])});
    }
    return m;
}

double MaxAxis(const float* ax, const float* bx) {
    return std::max({A(ax[0]), A(ax[1]), A(ax[2]), A(bx[0]), A(bx[1]), A(bx[2])});
}

/// Absolute error of one plane evaluation from denormal flushing.
double FlushError(const Plane& pl, double m, double axes) {
    return kAbs * (1.0 + A(pl.x) + A(pl.y) + A(pl.z) + A(pl.w) + m + axes);
}

/// After an undecidable plane k0: the GPU's segments at the later planes lie within dev0 plus the rounding of their
/// clips. Culled only when both ends are outside a later plane by more than that bound.
Result Superset(const Plane* planes, uint32_t k0, uint32_t count, const float* ax, const float* bx, const float* p,
                const float* q, float wp, float wq, double dev0) {
    const double m = MaxCoord(p, q) + dev0;
    const double axes = MaxAxis(ax, bx);
    if (!std::isfinite(m) || !std::isfinite(dev0) || !std::isfinite(axes)) {
        return Result::Kept;
    }
    for (uint32_t j = k0 + 1; j < count; ++j) {
        const Plane& pl = planes[j];
        const PlaneEval e = EvalPlane(pl, ax, bx, p, q, wp, wq);
        const double n1 = A(pl.x) + A(pl.y) + A(pl.z);
        // Each GPU clip since k0 rounds (8 kRel m) and may flush (kAbs (1 + m), s and P - Q).
        const double dev = dev0 + double(j - k0) * (8.0 * kRel * m + kAbs * (1.0 + 2.0 * m));
        const double tol = kChain * kRel * (2.0 * std::max(e.lQ, e.lP) + n1 * dev) + n1 * dev +
                           kChain * FlushError(pl, m + dev, axes);
        if (std::isfinite(tol) && double(e.dQ) < -tol && double(e.dP) < -tol) {
            return Result::Culled;
        }
    }
    return Result::Kept;
}

enum class Quick { Out, In, Unsure };
Quick QuickClassify(const Plane* planes, uint32_t count, const float* ax, const float* bx, const float* p,
                    const float* q);

/// The plane loop of one candidate object: Culled / Visible exactly as the GPU decides, or Kept when the GPU's
/// decision could not be established (it may have culled).
Result EvaluateSection(const uint32_t* sec, const Plane* planes, uint32_t count) {
    float p[3], q[3], ax[3], bx[3];
    double dev = 0.0;
    for (int i = 0; i < 3; ++i) {
        const float x = F(sec[4 * i + 0]);
        const float y = F(sec[4 * i + 1]);
        const float z = F(sec[4 * i + 2]);
        const float w = F(sec[4 * i + 3]);
        p[i] = w - x; // E = t - c0 (the shader's %259 / %262 / %265)
        q[i] = x + w; // S = t + c0 (%266 / %267 / %268)
        ax[i] = y;    // c1
        bx[i] = z;    // c2
        dev = std::max(dev, 2.0 * kRel * (A(x) + A(w)) + kAbs * (1.0 + A(x) + A(w)));
    }
    switch (QuickClassify(planes, count, ax, bx, p, q)) {
    case Quick::Out:
        return Result::Culled;
    case Quick::In:
        return Result::Visible;
    case Quick::Unsure:
        break;
    }
    const double axes = MaxAxis(ax, bx);
    // Bounds on |GPU end - our end|, per coordinate.
    double dev_p = dev;
    double dev_q = dev;
    float wp = 1.0f;
    float wq = 1.0f;
    for (uint32_t k = 0; k < count; ++k) {
        const Plane& pl = planes[k];
        const PlaneEval e = EvalPlane(pl, ax, bx, p, q, wp, wq);
        const double n1 = A(pl.x) + A(pl.y) + A(pl.z);
        const double m = MaxCoord(p, q) + std::max(dev_p, dev_q);
        const double flush = kChain * FlushError(pl, m, axes);
        // The GPU evaluates d at its own ends: its rounding scales with its magnitudes, which exceed ours by n1 dev.
        const double tol_q = kChain * kRel * (e.lQ + n1 * dev_q) + n1 * dev_q + flush;
        const double tol_p = kChain * kRel * (e.lP + n1 * dev_p) + n1 * dev_p + flush;
        const double dq = e.dQ;
        const double dp = e.dP;
        // NaN compares false: never inside nor outside, so undecidable.
        const bool in_q = dq > tol_q;
        const bool out_q = dq < -tol_q;
        const bool in_p = dp > tol_p;
        const bool out_p = dp < -tol_p;
        if (out_q && out_p) {
            return Result::Culled;
        }
        if (!(in_q || out_q) || !(in_p || out_p) || !std::isfinite(tol_q + tol_p)) {
            return Superset(planes, k, count, ax, bx, p, q, wp, wq, std::max(dev_p, dev_q));
        }
        const bool straddle = in_q != in_p; // %372
        float s = 0.0f;                     // %374
        double next_dev_p = dev_q + kAbs;   // both inside: P' = Q (flushed if denormal)
        if (straddle) {
            const double b = kChain * kRel * (DenScale(pl, p, q, wp, wq) + 2.0 * n1 * (dev_p + dev_q)) +
                             n1 * (dev_p + dev_q) + flush;
            if (!std::isfinite(e.den) || !(std::fabs(double(e.den)) > 2.0 * b)) {
                return Superset(planes, k, count, ax, bx, p, q, wp, wq, std::max(dev_p, dev_q));
            }
            const float rcp = 1.0f / e.den; // %362
            const float t = rcp * e.dQ;     // %366
            // NMax / NMin: a NaN operand yields the other one.
            const float lo = std::isnan(t) ? 0.0f : std::max(t, 0.0f); // %367
            s = 1.0f * std::min(lo, 1.0f);                              // %368, %374
            // |t_gpu - t| for dQ off by tol_q, den off by b, and the device's reciprocal.
            const double at = A(t);
            // A flushed (denormal) dQ moves t by up to |rcp| 2^-126.
            const double dt = 1.001 * (tol_q + at * b) / (std::fabs(double(e.den)) - b) + at * (kRcp + 2.0 * kRel) +
                              A(rcp) * 0x1p-126 + kAbs;
            if (!std::isfinite(at) || !(dt < 1.0)) {
                return Superset(planes, k, count, ax, bx, p, q, wp, wq, std::max(dev_p, dev_q));
            }
            double diff = 0.0;
            for (int i = 0; i < 3; ++i) {
                diff = std::max(diff, std::fabs(double(p[i]) - double(q[i])));
            }
            const double s_max = std::min(1.0, double(s) + dt);
            // Rounding of the GPU's three operations scales with its magnitudes (ours + dev); flushing s or P - Q
            // moves the product by < 2^-126 (1 + diff).
            next_dev_p = (dev_p + dev_q) * s_max + diff * dt + dev_q + 8.0 * kRel * m + kAbs * (1.0 + diff);
        }
        // %379: Q is kept when the segment straddles and Q is the inside end (dQ >= dP).
        const bool keep_q = straddle && dq >= dp;
        const float nwp = (wp - wq) * s + wq; // %375..%377
        const float nwq = keep_q ? wq : wp;   // %384
        for (int i = 0; i < 3; ++i) {
            const float np = (p[i] - q[i]) * s + q[i]; // %385..%387, %389..%392, %393..%395
            const float nq = keep_q ? q[i] : p[i];     // %388, %396, %397
            p[i] = np;
            q[i] = nq;
        }
        wp = nwp;
        wq = nwq;
        const double next_dev_q = keep_q ? dev_q : dev_p;
        dev_p = next_dev_p;
        dev_q = next_dev_q;
        // %390: (in_q || in_p) holds here, the visibility flag stays up.
    }
    return Result::Visible;
}


/// Cheap float classification of an object before the error model of EvaluateSection (plain float arithmetic: its
/// rounding is ~1e-7 of the magnitudes, the margin is 1e-2 of them, and the loop's own tolerance is ~6e-6 of them plus
/// n1 * dev with dev ~ 1e-6 * the coordinates).
///   Out:  at some plane both ends of the original segment are outside by the margin. Every point of the segment is
///         outside that plane (the plane value is linear), and every clipped segment the GPU tests lies on the
///         original one, so the GPU clears the object: Culled. (The full loop could return Kept when an earlier
///         plane was undecidable; Out is the sharper and still sound answer.)
///   In:   at every plane both ends are inside by the margin. Nothing is clipped (no plane straddles, s = 0, P and Q
///         only swap), every plane is decided "inside", and the loop returns Visible: the same answer.
///   Unsure: anything else (including NaN): the full loop decides.
Quick QuickClassify(const Plane* planes, uint32_t count, const float* ax, const float* bx, const float* p,
                    const float* q) {
    float m = 0.0f;
    for (int i = 0; i < 3; ++i) {
        m = std::max({m, std::fabs(p[i]), std::fabs(q[i])});
    }
    if (!(m < 1e30f)) { // also false for NaN
        return Quick::Unsure;
    }
    bool all_in = true;
    for (uint32_t k = 0; k < count; ++k) {
        const Plane& pl = planes[k];
        const float a1 = pl.x * ax[0], a2 = pl.y * ax[1], a3 = pl.z * ax[2];
        const float b1 = pl.x * bx[0], b2 = pl.y * bx[1], b3 = pl.z * bx[2];
        const float r = std::fabs(a1 + a2 + a3) + std::fabs(b1 + b2 + b3);
        const float dQ = pl.w + (pl.z * q[2] + (pl.y * q[1] + (pl.x * q[0] + r)));
        const float dP = pl.w + (pl.z * p[2] + (pl.y * p[1] + (pl.x * p[0] + r)));
        const float ax_ = std::fabs(pl.x), ay_ = std::fabs(pl.y), az_ = std::fabs(pl.z);
        const float mag = std::fabs(a1) + std::fabs(a2) + std::fabs(a3) + std::fabs(b1) + std::fabs(b2) +
                          std::fabs(b3) + ax_ * (std::fabs(p[0]) + std::fabs(q[0])) +
                          ay_ * (std::fabs(p[1]) + std::fabs(q[1])) + az_ * (std::fabs(p[2]) + std::fabs(q[2])) +
                          std::fabs(pl.w);
        const float margin = 1e-2f * (mag + (ax_ + ay_ + az_) * m);
        if (dQ < -margin && dP < -margin) {
            return Quick::Out;
        }
        if (!(dQ > margin && dP > margin)) {
            all_in = false;
        }
    }
    return all_in ? Quick::In : Quick::Unsure;
}

/// Counters of one CullShadow call, added to g_dbg once at the end (the shared atomics were a cache-line ping-pong
/// between the job threads when bumped per object).
struct LocalDebug {
    uint64_t objects = 0, gated = 0, lod_reject = 0, geometry_culled = 0, state_refused = 0, no_bit = 0, bit_a = 0,
             bit_b = 0;
    ~LocalDebug() {
        g_dbg.objects.fetch_add(objects, std::memory_order_relaxed);
        g_dbg.gated.fetch_add(gated, std::memory_order_relaxed);
        g_dbg.lod_reject.fetch_add(lod_reject, std::memory_order_relaxed);
        g_dbg.geometry_culled.fetch_add(geometry_culled, std::memory_order_relaxed);
        g_dbg.state_refused.fetch_add(state_refused, std::memory_order_relaxed);
        g_dbg.no_bit.fetch_add(no_bit, std::memory_order_relaxed);
        g_dbg.bit_a.fetch_add(bit_a, std::memory_order_relaxed);
        g_dbg.bit_b.fetch_add(bit_b, std::memory_order_relaxed);
    }
};

} // namespace

bool CullShadow(uint64_t owner, uint64_t cull_block, uint32_t index, uint32_t count, uint32_t* const masks[2]) {
    if (owner == 0 || cull_block == 0 || count == 0 || masks == nullptr || masks[0] == nullptr ||
        masks[1] == nullptr) {
        return false;
    }
    // The fade slot the producer writes into the block: cascade + 1 for index 0..3, 0 for the scenes.
    const uint32_t slot = index < 4 ? index + 1 : 0;
    g_dbg.calls.fetch_add(1, std::memory_order_relaxed);

    // The block as the dispatch reads it (the producer's stack copy, identical to the GPU copy).
    uint8_t raw[kBlockBytes];
    if (!SotcGuest::Read(cull_block, raw, sizeof raw)) {
        return false;
    }
    const uint32_t objects = U(raw + 0x00);
    const uint32_t mode = U(raw + 0x04);
    const uint32_t planes = U(raw + 0x08);
    const uint32_t block_slot = U(raw + 0x0c);
    const uint32_t klass = U(raw + 0x118);
    if (objects != count || block_slot != slot || planes > kMaxPlanes) {
        return false;
    }

    // The set's inputs: records, the LOD table, the counts. Every range must be mapped (the buffer cache's rule).
    uint64_t set = 0;
    if (!SotcGuest::Read(owner + kOwnerSetOffset, &set) || set == 0) {
        return false;
    }
    uint16_t set_count = 0;
    uint16_t model_count = 0;
    uint64_t records = 0;
    uint64_t lod = 0;
    if (!SotcGuest::Read(set + kSetCountOffset, &set_count) ||
        !SotcGuest::Read(set + kSetModelCountOffset, &model_count) ||
        !SotcGuest::Read(set + kSetRecordsOffset, &records) || !SotcGuest::Read(set + kSetLodOffset, &lod)) {
        return false;
    }
    if (set_count != count || model_count == 0) {
        return false;
    }
    const uint8_t* rec = SotcGuest::Backing(records, size_t{count} * kRecordBytes);
    const uint8_t* lods = SotcGuest::Backing(lod, size_t{model_count} * kLodRecordBytes);
    if (rec == nullptr || lods == nullptr) {
        return false;
    }
    if (g_dbg.logs.load(std::memory_order_relaxed) < 6 && g_dbg.logs.fetch_add(1) < 6) {
        // diagnostic: which V# of the owner is the 64-byte LOD table the shader reads dword 1 of (set+0 is assumed).
        uint64_t v[5] = {};
        const uint64_t offs[5] = {0x30, 0x58, 0xd0, 0x80, 0xa8};
        for (int i = 0; i < 5; ++i) {
            uint64_t w = 0;
            SotcGuest::Read(owner + offs[i], &w);
            v[i] = w & 0x00000fffffffffffull;
        }
        uint32_t m0[2] = {}, m1[2] = {};
        std::memcpy(m0, lods, 8);
        std::memcpy(m1, lods + kLodRecordBytes, 8);
        std::fprintf(stderr,
                     "SOTC CPUCULL SHADOW inputs: owner %#llx index %u set %#llx records %#llx set+0 %#llx | V# bases "
                     "+0x30 %#llx +0x58 %#llx +0xd0 %#llx +0x80 %#llx +0xa8 %#llx | models %u lod table dword0/1 of "
                     "model 0: %u %u, model 1: %u %u\n",
                     (unsigned long long)owner, index, (unsigned long long)set, (unsigned long long)records,
                     (unsigned long long)lod, (unsigned long long)v[0], (unsigned long long)v[1],
                     (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[4], model_count, m0[0],
                     m0[1], m1[0], m1[1]);
    }

    // Planes at +0x10, 16 bytes each (the shader's dwords 4 + 4k .. 7 + 4k). Read from the block as it is at the dispatch,
    // so planes past the 16th read the bytes the producer's copy left there, as the GPU's copy does.
    Plane plane_list[kMaxPlanes];
    for (uint32_t k = 0; k < planes; ++k) {
        const uint8_t* d = raw + 0x10 + 16 * size_t{k};
        plane_list[k] = {F(U(d + 0)), F(U(d + 4)), F(U(d + 8)), F(U(d + 12))};
    }

    // Both outputs, as the shader writes them: every word of the set's dword count, zero where nothing is set.
    const uint32_t words = (count + 31) / 32;
    std::memset(masks[0], 0, size_t{words} * 4);
    std::memset(masks[1], 0, size_t{words} * 4);

    LocalDebug dbg;
    std::optional<OwnerLod> mirror; // the LOD mirror of the owner, pinned on first use
    for (uint32_t s = 0; s < count; ++s) {
        uint32_t sec[16];
        std::memcpy(sec, rec + size_t{s} * kRecordBytes, kRecordBytes);
        ++dbg.objects;
        if ((mode & sec[14]) == 0 || (klass & sec[15]) == 0) {
            ++dbg.gated;
            continue;
        }
        const uint32_t object = sec[12];
        if (object >= model_count) {
            return false;
        }
        if (sec[13] != U(lods + size_t{object} * kLodRecordBytes + 4)) {
            ++dbg.lod_reject;
            continue;
        }
        const Result result = EvaluateSection(sec, plane_list, planes);
        if (result == Result::Culled) {
            ++dbg.geometry_culled;
            continue;
        }
        if (!mirror) {
            mirror.emplace(owner, slot);
        }
        ModelBits state;
        if (!mirror->ok() || !mirror->Get(object, &state)) {
            ++dbg.state_refused;
            return false;
        }
        const float f = state.fade;
        // The GPU's bit is set when its own fade lies on that side of the threshold. Our fade may be off by kFadeEps:
        // a bit is set whenever the GPU's fade may lie there.
        const double fd = double(f);
        const bool may_full = fd + kFadeEps > double(kFadeFull);                       // GPU: 0.99 < f
        const bool may_fade = fd + kFadeEps > double(kFadeMin) && !(fd - kFadeEps > double(kFadeFull)); // 0.01 < f <= 0.99
        const uint32_t bit = uint32_t{1} << (s & 31);
        if (may_full) {
            masks[0][s >> 5] |= bit;
            ++dbg.bit_a;
        }
        if (may_fade) {
            masks[1][s >> 5] |= bit;
            ++dbg.bit_b;
        }
        if (!may_full && !may_fade) {
            ++dbg.no_bit;
        }
    }
    return true;
}

void ShadowDebugLine() {
    const auto take = [](std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.exchange(0)); };
    std::fprintf(stderr,
                 "SOTC CPUCULL SHADOW detail: calls %llu, objects %llu (mode/class gated %llu, lod rejected %llu, "
                 "geometry culled %llu, no fade bit %llu), bits A %llu B %llu, calls refused for an unproven LOD state "
                 "%llu\n",
                 take(g_dbg.calls), take(g_dbg.objects), take(g_dbg.gated), take(g_dbg.lod_reject),
                 take(g_dbg.geometry_culled), take(g_dbg.no_bit), take(g_dbg.bit_a), take(g_dbg.bit_b),
                 take(g_dbg.state_refused));
}

} // namespace SotcCpuCull
