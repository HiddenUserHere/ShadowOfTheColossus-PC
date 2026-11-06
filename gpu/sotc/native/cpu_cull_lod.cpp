// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU mirror of the per-model LOD and fade pre-shader (eboot 0x19b400, compute shader
// user/cache/CUSA08809/0x000000007b79db7c_0.spv, local size 64). One invocation per model (gid = model index).
// The shader writes table58 (32 bytes per model, owner+0x58 V#) and table_d0 (8 bytes per model, owner+0xd0 V#).
// The main cull (2e0f474b) and the shadow cull (4defcf7f) read them (cpu_cull_main.cpp, cpu_cull_shadow.cpp).
//
// Evidence for the dispatch (decomp 0x19b400 and the disassembly of the eboot, 1 Nov):
//   - 0x19b400 builds the 0x30-byte header at [rbp-0x160] (copied to the ssbo_1 V# by 0x8be60 at 0x19b507), binds
//     the V#s (0x861e60 / 0x8619a0 / 0x861ce0 with the resource maps of the pipeline) and calls 0x85f220
//     (Material_BindComputeShader) and then the dispatch 0x86b9e0 with group count (count + 63) / 64.
//   - Bindings of the SPIR-V (OpDecorate): ssbo_1 = binding 0 header (RO), ssbo_2 = binding 1 model records (RO),
//     ssbo_3 = binding 2 scene constant block (RO), ssbo_4 = binding 3 table_d0 (RW), ssbo_5 = binding 4 table58
//     (RW), srt_flatbuf = binding 5 (RO, flat user data, dword 10 = the model capacity gate).
//   - The V#s: owner+0xa8 -> ssbo_2 (0x861ce0, resource class +0x22, index 0), [renderer+0x430] -> ssbo_3 (0x8619a0
//     index 1, class +0x82), {owner+0xd0, owner+0x58} -> ssbo_4, ssbo_5 (0x861e60 indices 0, 1, class +0x42), the
//     header -> ssbo_1 (0x8619a0 index 0, class +0x82).
//
// srt_flatbuf[10] (ud10, the guard "ud10 > gid" of the stores to table58[4], [6], [7]). The flat buffer starts with the
// 16 user data registers (Shader::Info::RefreshFlatBuf), so dword 10 is the user data register s10 of the dispatch
// and NOT dword 10 of the material's user-data table (the earlier mirror read that and got 0). Each setter
// (0x861ce0 / 0x861e60 / 0x8619a0) reads the 16-bit slot word of the resource at desc + class + 2 * index, with
// desc = [material + 0xc180] (stage 0, compute) and material = ctx + 0x2e8: a slot with bit 15 set is a DIRECT V#
// (sub_86d5e0 sets s[slot & 0x3fff .. +3] to the four V# dwords), otherwise the V# is copied into the table behind
// the user-data pointer. When s10 is the word 2 of a direct V# (num_records) the guard is "model < capacity".
// ResolveUd10 replays this selection and logs how it resolved (diagnostic lines); when it cannot, the owner is never
// served for that dispatch.
//
// Carried state. The shader reads the previous dispatch's table_d0[0] (LOD bits), table_d0[1] and table58[6]
// (fade progress). Its transition is
//     changed = x != lod; valid = x != -1.0
//     changed && valid  : d0 = (lod, x),  p = 1 - p
//     changed && !valid : d0 = (lod, y),  p = 1.0
//     !changed          : p = min(1, dt + p)
// (p only when ud10 > gid). The tables start with whatever the game left in them, so the mirror never assumes a
// start state: it is SEEDED from the GPU copy of the tables (native_copies.h) of the newest completed frame and
// stepped forward over the recorded dispatches since (the history keeps one small record per dispatch: the LODs,
// dt, ud10). Every completed GPU copy is also compared with the state the mirror predicted for that dispatch; an
// owner is served only after two consecutive agreeing frames and a proof that is not older than kMaxProofAge frames,
// so a wrong mirror (wrong ud10, another shader variant, a missed dispatch) falls back to the GPU copy instead of
// hiding objects.
//
// Float rules: the transcription keeps the shader's operation order. The build must not contract a*b+c into an
// FMA (-ffp-contract=off). LOD boundaries: the metric M is 1 / sqrt(d2) in float; the GPU inversesqrt may differ by
// a few ULP, so a model whose M lies within those ULPs of a threshold can get the other LOD. The compare counts
// those models; nothing else is approximated. The fade words are compared with an epsilon of 1e-5.
//
// Cost model (2 Nov rework; the earlier mirror ran ~1.4 M models per second through a full evaluation on the game's job
// threads and broke the frame pacing):
//   - The job thread (Dispatch) only computes the LOD index of every model and advances the carried state, because
//     the state of EVERY model must follow EVERY dispatch the GPU runs. That is a 4-wide SSE2 kernel (Lod4) whose
//     per-lane operations are the scalar ones (sqrtps / divps / mulps / addps are correctly rounded IEEE operations,
//     the same results as the scalar code; contraction is off), plus a skip of the state update for the models whose
//     state does not change (same LOD, fade progress saturated at 1). tests: the scalar EvalLod is the reference.
//   - The fade words (table58[0..4], [7]) are stateless functions of the dispatch inputs. They are no longer stored per
//     model: OwnerLod::Get evaluates the one word the cull reads, for the models the cull references, from the inputs
//     of the owner's newest dispatch (kept in OwnerState). Words the dispatch did not write (table58[4] and [7] when
//     ud10 <= model, [5] always) kept an older value on the GPU; the mirror does not track those, and Get fails there.
//   - The mirror is verified against the GPU tables by SAMPLING (see Dispatch): an owner whose mirror is proven is
//     registered for the copy ring at every dispatch, over a prefix window of at most kWindow
//     models; an unproven owner is registered in full. Every dispatch is registered, never a subset (the copy ring pairs
//     the GPU writes of a base with the registrations in order, and the draw thread may still be recording writes of
//     earlier frames, so a skipped dispatch would give its write to the next registration). The compare, the seed and the readout of the
//     copies run on a host worker thread (Worker), never on a game thread.
//   - Per-owner mutex (OwnerState::mu) instead of one global mutex; the global one only guards the owner map and the
//     pending queue, briefly. Lock order: OwnerState::mu, then g_mutex (never the other way round).
#pragma clang fp contract(off)

#include "cpu_cull.h"

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../guest_mem.h"
#include "../hooks.h"
#include "native.h"
#include "native_copies.h"
#include "native_masks.h"

namespace SotcCpuCull {
void ShadowDebugLine(); // cpu_cull_shadow.cpp: diagnostic counters of the shadow replay

namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

/// Inputs of one invocation. Dword indices refer to the shader's SSBOs (ssbo_N = binding N-1), see the header above.
struct LodInputs {
    float dt = 0;               ///< ssbo_1[1]: step added to the fade while the LOD is unchanged
    float speed = 0;            ///< ssbo_1[2]: used when > 0, otherwise rec[12] is the speed
    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0; ///< ssbo_1[8], [5], [6], [4], [7], [9] (see FadeWord)
    float cam[3] = {};          ///< ssbo_3[140..142]
    float scale = 0;            ///< ssbo_3[147] (s147)
    float metric_override = 0;  ///< ssbo_3[148] (s148): the metric M when > 0
    int32_t lod_override = -1;  ///< ssbo_3[151]: the LOD when >= 0
};

inline float NMax(float x, float y) {
    return std::isnan(x) ? y : (std::isnan(y) ? x : (x > y ? x : y));
}

inline float NMin(float x, float y) {
    return std::isnan(x) ? y : (std::isnan(y) ? x : (x < y ? x : y));
}

inline float Clamp01(float x) {
    return NMin(NMax(x, 0.0f), 1.0f);
}

inline uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

inline float Float(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

inline uint32_t Word(const uint8_t* rec, int i) {
    uint32_t u;
    std::memcpy(&u, rec + 4 * i, sizeof u);
    return u;
}

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

constexpr uint32_t kNoLod = 0xffu; // history marker: the model was not stepped (poisoned)

/// The two pre-shader variants the game dispatches for the same owners (told apart by the code address of the bound
/// compute program, see SotcNativeCopies::ProgramAt):
///   kStateful 0x7b79db7c: the crossfading shader described above (carried state x, y, p).
///   kSnap     0x6eb39c35: ssbo_4 is the 32-byte table, ssbo_5 the 8-byte one (swapped against kStateful). Per live
///             model it stores table_d0 = (lod, lod), table58[6] = 1.0, table58[7] = M (ungated), table58[0..3] as
///             kStateful and table58[4] when ud10 > gid. It reads no previous state and anchors the mirror.
enum class Kind : uint8_t { kUnknown, kStateful, kSnap };
constexpr uint64_t kHashStateful = 0x7b79db7c;
constexpr uint64_t kHashSnap = 0x6eb39c35;

// ---- The stateless parts of one invocation --------------------------------------------------------------------

inline float DistanceSquared(const LodInputs& in, const uint8_t* rec) {
    // d2 = dz*dz + (dy*dy + dx*dx), as the shader adds.
    const float dx = Float(Word(rec, 8)) - in.cam[0];
    const float dy = Float(Word(rec, 9)) - in.cam[1];
    const float dz = Float(Word(rec, 10)) - in.cam[2];
    return dz * dz + (dy * dy + dx * dx);
}

/// The metric M (table58[7]): s148 when > 0, else s147 * radius / dist.
inline float Metric(const LodInputs& in, const uint8_t* rec) {
    if (in.metric_override > 0.0f) {
        return in.metric_override;
    }
    const float d2 = DistanceSquared(in, rec);
    return in.scale * (Float(Word(rec, 11)) * (1.0f / std::sqrt(d2)));
}

/// The LOD index of one model: first k in [0, N-2] with M >= threshold(k), else N-1. threshold(k) = rec[4+k] for k in
/// 1..3, rec[4] otherwise (the shader's select chain falls back to rec[4] for k = 0 and for k >= 4). False when the
/// inputs leave the shader's behaviour undefined (N = 0: the GPU loop bound wraps) or the LOD does not fit the
/// history byte. This scalar form is the reference of Lod4.
bool EvalLod(const LodInputs& in, const uint8_t* rec, uint32_t* lod_out) {
    const uint32_t n = Word(rec, 0);
    if (n == 0) {
        return false;
    }
    const float M = Metric(in, rec);
    const uint32_t nm1 = n - 1;
    uint32_t k = 0;
    while (k < nm1) {
        const uint32_t thr_index = (k >= 1 && k <= 3) ? 4 + k : 4;
        if (M >= Float(Word(rec, static_cast<int>(thr_index)))) {
            break;
        }
        ++k;
    }
    uint32_t lod = k;
    if (in.lod_override >= 0) {
        lod = static_cast<uint32_t>(in.lod_override);
    }
    if (lod > nm1) {
        lod = nm1;
    }
    if (lod >= kNoLod) {
        return false;
    }
    *lod_out = lod;
    return true;
}

/// Four models at once (SSE2): the same operations as EvalLod in the same order, lane by lane. Fast domain: N in
/// 1..255 for every lane and no LOD override; false outside it (the caller steps the models one by one).
inline bool Lod4(const LodInputs& in, const uint8_t* recs, uint32_t m, uint32_t out[4]) {
    const uint8_t* r = recs + 64 * size_t{m};
    const uint32_t n0 = Word(r, 0), n1 = Word(r + 64, 0), n2 = Word(r + 128, 0), n3 = Word(r + 192, 0);
    if (n0 - 1u >= 255u || n1 - 1u >= 255u || n2 - 1u >= 255u || n3 - 1u >= 255u) {
        return false;
    }
    __m128 t0 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 16)); // words 4..7: the thresholds
    __m128 t1 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 80));
    __m128 t2 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 144));
    __m128 t3 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 208));
    _MM_TRANSPOSE4_PS(t0, t1, t2, t3); // t_j = threshold word 4 + j of the four lanes
    __m128 M;
    if (in.metric_override > 0.0f) {
        M = _mm_set1_ps(in.metric_override);
    } else {
        __m128 c0 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 32)); // words 8..11: centre xyz, radius
        __m128 c1 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 96));
        __m128 c2 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 160));
        __m128 c3 = _mm_loadu_ps(reinterpret_cast<const float*>(r + 224));
        _MM_TRANSPOSE4_PS(c0, c1, c2, c3); // c0 = x, c1 = y, c2 = z, c3 = radius
        const __m128 dx = _mm_sub_ps(c0, _mm_set1_ps(in.cam[0]));
        const __m128 dy = _mm_sub_ps(c1, _mm_set1_ps(in.cam[1]));
        const __m128 dz = _mm_sub_ps(c2, _mm_set1_ps(in.cam[2]));
        const __m128 d2 = _mm_add_ps(_mm_mul_ps(dz, dz), _mm_add_ps(_mm_mul_ps(dy, dy), _mm_mul_ps(dx, dx)));
        const __m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(d2));
        M = _mm_mul_ps(_mm_set1_ps(in.scale), _mm_mul_ps(c3, inv));
    }
    const __m128i nm1 = _mm_set_epi32(static_cast<int>(n3 - 1), static_cast<int>(n2 - 1), static_cast<int>(n1 - 1),
                                      static_cast<int>(n0 - 1));
    __m128i lod = nm1;
    const __m128 thr[4] = {t0, t1, t2, t3};
    for (int j = 3; j >= 0; --j) { // the lowest j that satisfies "j < N-1 and M >= thr(j)" wins
        const __m128i ge = _mm_castps_si128(_mm_cmpge_ps(M, thr[j])); // false for NaN, as the scalar >=
        const __m128i inside = _mm_cmpgt_epi32(nm1, _mm_set1_epi32(j));
        const __m128i hit = _mm_and_si128(ge, inside);
        lod = _mm_or_si128(_mm_and_si128(hit, _mm_set1_epi32(j)), _mm_andnot_si128(hit, lod));
    }
    alignas(16) uint32_t tmp[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(tmp), lod);
    std::memcpy(out, tmp, sizeof tmp);
    return true;
}

/// The fade word table58[slot] for slot 0..4 of one model (the shader's operation order). Fade shape:
/// F(x) = clamp01((dist - R / (x * s)) / (a * x)) for the four bands, with the band factors b (F1), d (F2), c (F3),
/// e (F5), and the plain band F4 = clamp01((dist - R / s) / a); the word is f * (1 - F) evaluated as (-f) * F + f.
/// Slot 0 = F4, 1 = F2, 2 = F1, 3 = F3, 4 = F5.
float FadeWord(const LodInputs& in, const uint8_t* rec, int slot) {
    const float dist = std::sqrt(DistanceSquared(in, rec));
    const float R = in.scale * Float(Word(rec, 11)); // s147 * radius
    const float s = (in.speed > 0.0f) ? in.speed : Float(Word(rec, 12)); // the header speed, else rec[12]
    const float a = in.a;
    float F;
    switch (slot) {
    case 0:
        F = Clamp01(((-(1.0f / s)) * R + dist) * (1.0f / a));
        break;
    case 1:
        F = Clamp01(((-(1.0f / (in.d * s))) * R + dist) * (1.0f / (a * in.d)));
        break;
    case 2:
        F = Clamp01(((-(1.0f / (in.b * s))) * R + dist) * (1.0f / (a * in.b)));
        break;
    case 3:
        F = Clamp01(((-(1.0f / (in.c * s))) * R + dist) * (1.0f / (a * in.c)));
        break;
    default:
        F = Clamp01(((-(1.0f / (in.e * s))) * R + dist) * (1.0f / (a * in.e)));
        break;
    }
    return (-in.f) * F + in.f;
}

// ---- The carried state ------------------------------------------------------------------------------------------

// Carried state of a model: x = table_d0[0] (the LOD, as a float), y = table_d0[1], p = table58[6]. See the header
// comment for the transition. Arrays of OwnerState.
struct StateRef {
    uint32_t* x;
    uint32_t* y;
    uint32_t* p;
};

/// The carried-state transition of one model for one dispatch.
inline void Advance(StateRef s, uint32_t lod, float dt, bool ud_ok) {
    const float lod_f = static_cast<float>(lod);
    const float px = Float(*s.x);
    const bool changed = px != lod_f;    // FUnordNotEqual: true for NaN too
    const bool prev_valid = px != -1.0f; // FUnordNotEqual against -1.0
    const float pf = Float(*s.p);
    if (changed) {
        if (prev_valid) {
            *s.y = *s.x;
            *s.x = Bits(lod_f);
            if (ud_ok) {
                *s.p = Bits(1.0f - pf); // the crossfade restarts from the mirrored progress
            }
        } else {
            *s.x = Bits(lod_f); // y keeps its value
            if (ud_ok) {
                *s.p = Bits(1.0f);
            }
        }
    } else if (ud_ok) {
        *s.p = Bits(NMin(1.0f, dt + pf));
    }
}

/// The kSnap transition: the previous state is not read.
inline void Snap(StateRef s, uint32_t lod) {
    *s.x = Bits(static_cast<float>(lod));
    *s.y = *s.x;
    *s.p = Bits(1.0f);
}

// ---- Mirror of the GPU tables ----------------------------------------------------------------------------------

// Guest offsets (eboot 0x19b400, see the header comment).
constexpr uint64_t kSceneGlobal = 0x2ee3ce0;      // DAT_02ee3ce0: the renderer pointer (host memory of the image)
constexpr uint64_t kRendererSceneV = 0x430;       // renderer+0x430: V# of the scene constant block (ssbo_3)
constexpr size_t kSceneDwords = 152;              // dwords 0..151 (151 = the LOD override)
constexpr int64_t kHeaderOffset = -0x160;         // [rbp-0x160]: the 0x30-byte header
constexpr int64_t kHeaderVOffset = -0x188;        // [rbp-0x188]: the header V#
constexpr uint64_t kOwnerSet = 0x18;              // owner+0x18: the set record (identity of the owner)
constexpr uint64_t kOwnerRecordsV = 0xa8;         // owner+0xa8: V# of the model records (ssbo_2)
constexpr uint64_t kOwnerTable58V = 0x58;         // owner+0x58: V# of table58 (ssbo_5)
constexpr uint64_t kOwnerTableD0V = 0xd0;         // owner+0xd0: V# of table_d0 (ssbo_4)
constexpr uint64_t kMaterialOffset = 0x2e8;       // material = ctx + 0x2e8
constexpr uint64_t kMaterialDesc = 0xc180;        // [material + 0xc180 + 8 * stage]: the resource descriptor
constexpr uint64_t kMaterialInline0 = 0xc0e0;     // inline user data dword sources (0x85f220)
constexpr uint64_t kMaterialInline1 = 0xc100;
constexpr uint64_t kRecordBytes = 64;
constexpr uint32_t kMaxModels = 65535;
constexpr uint64_t kVAddrMask = 0x00000fffffffffffull; // V# base: 44 bits (word1 bits 12+ hold the stride)
constexpr uint64_t kKeyLod = 3ull << 62;               // copy-ring namespace of this client (native_copies.h)
constexpr uint64_t kMaxAgeFrames = 12;                 // a pending compare older than this is dropped
constexpr uint64_t kMaxProofAge = 12;                  // frames an agreeing compare keeps the owner servable
constexpr uint32_t kNeedStreak = 2;                    // consecutive agreeing compares before an owner is served
constexpr uint32_t kWindow = 256;                      // models verified per dispatch of a proven owner (a prefix)
constexpr uint64_t kFullEvery = 24;                    // a proven owner is verified in full every 24th frame
constexpr size_t kMaxPending = 512;
constexpr size_t kMaxHistory = 512;                    // dispatch records kept per owner
constexpr uint64_t kHistoryFrames = 24;
constexpr float kFadeEpsilon = 1e-4f;
constexpr size_t kPoolMax = 6;                         // spare history vectors kept per owner
constexpr const char* kLodHook = "NATIVE_LOD_DISPATCH";
constexpr int kMaxMismatchLogs = 48; // diagnostic: model mismatches logged with CPU and GPU words
constexpr int kMaxUdLogs = 14;       // diagnostic: ud10 resolutions logged

/// One dispatch of an owner, kept to replay the carried state forward from a GPU seed.
struct DispatchRecord {
    uint64_t dseq = 0;
    uint64_t frame = 0;
    uint32_t count = 0;
    uint32_t dt_bits = 0;
    uint32_t ud10 = 0;
    bool ud_known = false; // the dispatch is modelled: ud10 and the variant are known
    Kind kind = Kind::kUnknown;
    std::vector<uint8_t> lod; // per model: the LOD index, kNoLod when the model was not stepped
};

struct OwnerState {
    std::shared_mutex mu; // exclusive: dispatch, verify, seed; shared: the cull reads (OwnerLod)
    uint64_t set = 0;     // identity: owner+0x18
    uint64_t base58 = 0;  // identity: owner+0x58 V# base
    uint64_t based0 = 0;  // identity: owner+0xd0 V# base
    uint64_t basea8 = 0;  // the model records of the newest dispatch (owner+0xa8 V# base)
    uint64_t pipe = 0;    // resource descriptor of the bound pre-shader (identifies the pipeline variant)
    uint32_t count = 0;   // model count of the last successful dispatch
    uint64_t frame = 0;   // native frame of the last successful dispatch; 0 = not usable this frame
    uint64_t dseq = 0;    // dispatch counter of the owner
    uint32_t streak = 0;  // consecutive agreeing compares
    uint64_t proof_frame = 0; // frame of the last agreeing compare
    // Carried state per model.
    std::vector<uint32_t> sx, sy, sp; // table_d0[0], table_d0[1], table58[6]
    std::vector<uint8_t> known;       // the state is anchored on a GPU copy and stepped on every dispatch since
    std::deque<DispatchRecord> history;
    std::vector<std::vector<uint8_t>> pool; // spare lod vectors
    // Inputs of the newest dispatch (the lazy fade words).
    LodInputs in;
    uint32_t ud10 = 0;
    bool ud_known = false;
    bool snap = false;
    // Verify sampling.
    uint64_t sample_frame = 0;      // the native frame whose dispatches are registered (0: none)
    uint64_t last_sample_frame = 0;
    uint32_t sample_len = 0;
    bool need_full = true;          // the next sample covers the whole table (state not fully anchored)
};

/// The mirror's expectation of one model after a sampled dispatch.
struct Row {
    uint32_t x = 0, y = 0, p = 0;
    uint32_t known = 0;
};

// A sampled dispatch waiting for its GPU copies. The copies are registered at the dispatch (SotcNativeCopies); only
// the window [first, first + n) of the tables is registered.
struct PendingCheck {
    uint64_t frame = 0;
    uint64_t dseq = 0;
    uint64_t key58 = 0;
    uint64_t keyd0 = 0;
    uint64_t owner = 0;
    uint64_t set = 0, base58 = 0, based0 = 0;
    uint64_t bytes58 = 0;
    uint64_t bytesd0 = 0;
    uint32_t total = 0;  // the dispatch's model count
    uint32_t first = 0;  // window
    uint32_t n = 0;
    uint64_t pipe = 0;
    uint32_t ud10 = 0;
    bool ud_known = false;
    Kind kind = Kind::kUnknown;
    bool superseded = false;   // a later dispatch of the owner in the same frame owns the copy
    LodInputs in;
    std::vector<Row> expect;           // the mirror's state after this dispatch, per window model
    std::vector<uint8_t> lod;          // the LOD the mirror computed per window model (kNoLod: none)
    std::vector<uint8_t> recs;         // the window's model records at the dispatch (the fade words are computed from them)
    std::vector<uint8_t> got58;
    std::vector<uint8_t> gotd0;
};

struct Counters {
    std::atomic<uint64_t> dispatches{0};
    std::atomic<uint64_t> models{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> lod_mismatch{0};
    std::atomic<uint64_t> fade_mismatch{0};
    std::atomic<uint64_t> verified{0};
    std::atomic<uint64_t> unverified{0};
    std::atomic<uint64_t> sampled{0};
    // Diagnostic detail counters (the "SOTC CPUCULL LOD" line).
    std::atomic<uint64_t> bad_x{0}, bad_y{0}, bad_p{0}, bad_fade{0};
    std::atomic<uint64_t> compares{0}, agree{0}, disagree{0};
    std::atomic<uint64_t> seeded{0}, seed_skipped{0}, seed_avoided{0};
    std::atomic<uint64_t> refused_unproven{0}, refused_unknown{0}, refused_stale{0};
    std::atomic<uint64_t> ud_unresolved{0}, pipe_changed{0}, poisoned{0};
    std::atomic<uint64_t> served_models{0};
    std::atomic<uint64_t> simd_groups{0}, simd_skipped{0}, scalar_models{0};
    std::atomic<uint64_t> word_bad[9]{};
    std::atomic<uint64_t> kind_dispatches[3]{};
    std::atomic<uint64_t> pipe_models[8]{};
    std::atomic<uint64_t> pipe_bad[8]{};
};
Counters g_c;
std::atomic<uint64_t> g_stage_ns[static_cast<int>(Stage::kCount)];

std::mutex g_pipe_mutex; // leaf lock: g_pipe_ids
uint64_t g_pipe_ids[8] = {}; // descriptors seen, in order of appearance

int PipeSlot(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_pipe_mutex);
    for (int i = 0; i < 8; ++i) {
        if (g_pipe_ids[i] == id) {
            return i;
        }
        if (g_pipe_ids[i] == 0) {
            g_pipe_ids[i] = id;
            return i;
        }
    }
    return 7;
}

std::mutex g_mutex; // guards g_owners (the map, not the states) and g_pending
std::unordered_map<uint64_t, OwnerState> g_owners; // node-based: a state never moves
std::deque<PendingCheck> g_pending;
std::mutex g_drain_mutex; // one drainer at a time (the worker, or the report)
std::atomic<int> g_mismatch_logs{0};
std::atomic<int> g_ud_logs{0};

OwnerState* FindOwner(uint64_t owner, bool create) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (create) {
        return &g_owners[owner];
    }
    const auto it = g_owners.find(owner);
    return it == g_owners.end() ? nullptr : &it->second;
}

uint64_t Ptr(uint64_t address) {
    uint64_t v = 0;
    return SotcGuest::Read(address, &v) ? v : 0;
}

// The base address of the V# at vaddr (word0/word1 low 44 bits); 0 when unreadable.
uint64_t VBase(uint64_t vaddr) {
    return Ptr(vaddr) & kVAddrMask;
}

uint32_t LoadU32(const uint8_t* bytes, size_t index) {
    uint32_t u;
    std::memcpy(&u, bytes + 4 * index, sizeof u);
    return u;
}

// Fade words compare with an epsilon; NaN equals NaN only.
// The tolerance is absolute for the fades in [0, 1] (the GPU's inversesqrt / sqrt / rcp differ from the exact ones by
// a few ULP of the distance, which a fade sees divided by the band width) and relative for the metric (up to the
// thousands: one ULP there is 1e-4).
bool FadeDiffers(float expect, float got) {
    const bool en = std::isnan(expect);
    const bool gn = std::isnan(got);
    if (en || gn) {
        return en != gn;
    }
    const float scale = std::fmax(std::fabs(expect), std::fabs(got));
    return std::fabs(expect - got) > kFadeEpsilon + 1e-5f * scale;
}

// ---- srt_flatbuf[10] -------------------------------------------------------------------------------------------

struct UdResult {
    bool known = false;
    uint32_t value = 0;
    char how[160] = {};
};

/// Replays the user-data setup of the pre-shader dispatch to find the value of the user data register s10 (see the
/// header comment). `ctx` is the command-list context (rbx at the hook), `list` r13 (its +0x20 object holds the
/// stage index), `rbp` the frame of 0x19b400. `text`: fill UdResult::how (only for the first few logged dispatches).
UdResult ResolveUd10(uint64_t ctx, uint64_t list, uint64_t owner, uint64_t rbp, uint64_t renderer, bool text) {
    UdResult r;
    const uint64_t material = ctx + kMaterialOffset;
    uint32_t stage = 0;
    const uint64_t stage_obj = Ptr(list + 0x20);
    if (stage_obj != 0) {
        SotcGuest::Read(stage_obj + 0x14, &stage);
    }
    if (stage != 0) {
        if (text) {
            std::snprintf(r.how, sizeof r.how, "stage %u (not compute)", stage);
        }
        return r;
    }
    const uint64_t desc = Ptr(material + kMaterialDesc);
    uint8_t d[0xa0] = {};
    if (desc == 0 || !SotcGuest::Read(desc, d, sizeof d)) {
        if (text) {
            std::snprintf(r.how, sizeof r.how, "descriptor %#llx unreadable", (unsigned long long)desc);
        }
        return r;
    }
    struct Src {
        uint32_t base;
        uint32_t index;
        uint64_t vaddr;
        const char* name;
    };
    const Src srcs[] = {
        {0x22, 0, owner + kOwnerRecordsV, "records"},
        {0x42, 0, owner + kOwnerTableD0V, "d0"},
        {0x42, 1, owner + kOwnerTable58V, "t58"},
        {0x82, 0, rbp + static_cast<uint64_t>(kHeaderVOffset), "header"},
        {0x82, 1, renderer ? renderer + kRendererSceneV : 0, "scene"},
    };
    char slots[96] = {};
    int n = 0;
    for (const Src& s : srcs) {
        uint16_t w;
        std::memcpy(&w, d + s.base + 2 * s.index, sizeof w);
        if (text) {
            n += std::snprintf(slots + n, sizeof slots - n, "%s=%04x ", s.name, w);
            if (n >= static_cast<int>(sizeof slots) - 12) {
                n = static_cast<int>(sizeof slots) - 12;
            }
        }
        if (w == 0xffff || !(w & 0x8000)) {
            continue;
        }
        const uint32_t reg = w & 0x3fff;
        if (reg <= 10 && 10 < reg + 4 && s.vaddr != 0) {
            uint32_t v[4];
            if (SotcGuest::Read(s.vaddr, v, sizeof v)) {
                r.known = true;
                r.value = v[10 - reg];
                if (text) {
                    std::snprintf(r.how, sizeof r.how, "direct V# %s at s%u word %u [%s]", s.name, reg, 10 - reg, slots);
                }
                return r;
            }
        }
    }
    // Inline dwords and pointer pairs of 0x85f220: bytes 7..0xc are pointer registers (two registers each), 0xd and 0xe
    // inline single dwords.
    for (int i = 0xd; i <= 0xe; ++i) {
        if (d[i] == 10) {
            uint32_t v = 0;
            if (SotcGuest::Read(material + (i == 0xd ? kMaterialInline0 : kMaterialInline1), &v)) {
                r.known = true;
                r.value = v;
                if (text) {
                    std::snprintf(r.how, sizeof r.how, "inline dword (desc byte %#x) [%s]", i, slots);
                }
                return r;
            }
        }
    }
    for (int i = 7; i <= 0xc; ++i) {
        if (d[i] == 9 || d[i] == 10) {
            if (text) {
                std::snprintf(r.how, sizeof r.how, "s10 is half of a pointer (desc byte %#x = %u) [%s]", i, d[i], slots);
            }
            return r;
        }
    }
    if (text) {
        std::snprintf(r.how, sizeof r.how, "s10 not set by this dispatch [%s]", slots);
    }
    return r;
}

// ---- Compare, seed, replay -------------------------------------------------------------------------------------

// Bit k of the mask: 0..4 table58[k], 5 table58[7], 6 d0[0], 7 d0[1], 8 table58[6] (p).
constexpr int kWordKinds = 9;
constexpr const char* kWordName[kWordKinds] = {"t58[0]", "t58[1]", "t58[2]", "t58[3]", "t58[4]",
                                               "t58[7]", "d0[0]",  "d0[1]",  "t58[6]"};

void LogMismatch(const PendingCheck& p, uint32_t i, uint32_t mask, const float fades[6]) {
    if (g_mismatch_logs.load(std::memory_order_relaxed) >= kMaxMismatchLogs) {
        return;
    }
    const int n = g_mismatch_logs.fetch_add(1, std::memory_order_relaxed);
    if (n >= kMaxMismatchLogs) {
        return;
    }
    const Row& e = p.expect[i];
    const uint8_t* g58 = p.got58.data() + 32 * size_t{i};
    const uint32_t cpu[kWordKinds] = {Bits(fades[0]), Bits(fades[1]), Bits(fades[2]), Bits(fades[3]), Bits(fades[4]),
                                      Bits(fades[5]), e.x,           e.y,           e.p};
    const uint32_t gpu[kWordKinds] = {LoadU32(g58, 0), LoadU32(g58, 1), LoadU32(g58, 2),
                                      LoadU32(g58, 3), LoadU32(g58, 4), LoadU32(g58, 7),
                                      LoadU32(p.gotd0.data(), 2 * size_t{i}),
                                      LoadU32(p.gotd0.data(), 2 * size_t{i} + 1), LoadU32(g58, 6)};
    const uint8_t* rec = p.recs.data() + kRecordBytes * size_t{i};
    std::fprintf(stderr,
                 "SOTC CPUCULL LOD mismatch #%d: owner %#llx frame %llu dseq %llu pipe %#llx kind %d model %u/%u ud10 "
                 "%u known %u | rec[0]=%u rec[1]=%u rec[12]=%g thr=%g %g %g %g | lod %u\n",
                 n, (unsigned long long)p.owner, (unsigned long long)p.frame, (unsigned long long)p.dseq,
                 (unsigned long long)p.pipe, static_cast<int>(p.kind), p.first + i, p.total, p.ud10, e.known,
                 Word(rec, 0), Word(rec, 1), Float(Word(rec, 12)), Float(Word(rec, 4)), Float(Word(rec, 5)),
                 Float(Word(rec, 6)), Float(Word(rec, 7)), p.lod[i]);
    for (int k = 0; k < kWordKinds; ++k) {
        if (mask & (1u << k)) {
            std::fprintf(stderr, "    %s: cpu %.9g (%08x) gpu %.9g (%08x)\n", kWordName[k], Float(cpu[k]), cpu[k],
                         Float(gpu[k]), gpu[k]);
        }
    }
}

/// Compares one dispatch's window with its GPU copies. Counts per model and per word and updates the owner's streak.
/// `unknown_valid` gets the number of models the dispatch stepped whose carried state is not anchored. Returns true
/// when the mirror agreed with the GPU on every model it had a known state for. Owner mutex held.
bool Compare(const PendingCheck& p, OwnerState* st, uint32_t* unknown_valid) {
    uint64_t bad_state = 0;
    uint64_t bad_fade = 0;
    uint64_t compared_known = 0;
    uint32_t unknown = 0;
    for (uint32_t i = 0; i < p.n; ++i) {
        if (p.lod[i] == kNoLod) {
            continue;
        }
        const uint32_t m = p.first + i;
        const Row& e = p.expect[i];
        const uint8_t* g58 = p.got58.data() + 32 * size_t{i};
        const uint8_t* rec = p.recs.data() + kRecordBytes * size_t{i};
        const bool ud_ok = p.ud_known && p.ud10 > m;
        const bool w7 = ud_ok || p.kind == Kind::kSnap; // the words this variant writes for the model

        float fades[6] = {};
        uint32_t fade_mask = 0;
        for (uint32_t k = 0; k < 4; ++k) {
            fades[k] = FadeWord(p.in, rec, static_cast<int>(k));
            if (FadeDiffers(fades[k], Float(LoadU32(g58, k)))) {
                fade_mask |= 1u << k;
            }
        }
        if (ud_ok) {
            fades[4] = FadeWord(p.in, rec, 4);
            if (FadeDiffers(fades[4], Float(LoadU32(g58, 4)))) {
                fade_mask |= 1u << 4;
            }
        }
        if (w7) {
            fades[5] = Metric(p.in, rec);
            if (FadeDiffers(fades[5], Float(LoadU32(g58, 7)))) {
                fade_mask |= 1u << 5;
            }
        }
        uint32_t mask = fade_mask;
        if (fade_mask != 0) {
            ++bad_fade;
            g_c.bad_fade.fetch_add(1, std::memory_order_relaxed);
        }
        if (e.known) {
            ++compared_known;
            const uint32_t gx = LoadU32(p.gotd0.data(), 2 * size_t{i});
            const uint32_t gy = LoadU32(p.gotd0.data(), 2 * size_t{i} + 1);
            const bool bx = gx != e.x;
            const bool by = gy != e.y;
            const bool bp = FadeDiffers(Float(e.p), Float(LoadU32(g58, 6)));
            if (bx || by || bp) {
                ++bad_state;
                g_c.bad_x.fetch_add(bx, std::memory_order_relaxed);
                g_c.bad_y.fetch_add(by && !bx, std::memory_order_relaxed);
                g_c.bad_p.fetch_add(bp && !bx && !by, std::memory_order_relaxed);
                mask |= (bx ? 1u << 6 : 0u) | (by ? 1u << 7 : 0u) | (bp ? 1u << 8 : 0u);
            }
        } else {
            ++unknown;
        }
        if (mask != 0) {
            for (int k = 0; k < kWordKinds; ++k) {
                if (mask & (1u << k)) {
                    g_c.word_bad[k].fetch_add(1, std::memory_order_relaxed);
                }
            }
            LogMismatch(p, i, mask, fades);
        }
    }
    g_c.verified.fetch_add(p.n, std::memory_order_relaxed);
    g_c.lod_mismatch.fetch_add(bad_state, std::memory_order_relaxed);
    g_c.fade_mismatch.fetch_add(bad_fade, std::memory_order_relaxed);
    g_c.compares.fetch_add(1, std::memory_order_relaxed);
    {
        const int slot = PipeSlot(p.pipe);
        g_c.pipe_models[slot].fetch_add(p.n, std::memory_order_relaxed);
        g_c.pipe_bad[slot].fetch_add(bad_state + bad_fade, std::memory_order_relaxed);
    }

    const bool agree = bad_state == 0 && bad_fade == 0 && p.ud_known;
    if (agree && compared_known > 0) {
        ++st->streak;
        st->proof_frame = p.frame;
        g_c.agree.fetch_add(1, std::memory_order_relaxed);
    } else if (!agree) {
        st->streak = 0;
        g_c.disagree.fetch_add(1, std::memory_order_relaxed);
    }
    *unknown_valid = unknown;
    return agree;
}

/// Anchors the window's carried state on the GPU truth of dispatch p.dseq and steps it forward over the recorded
/// dispatches since. Models outside the window keep their state. Owner mutex held.
void Seed(const PendingCheck& p, OwnerState* st) {
    if (st->set != p.set || st->base58 != p.base58 || st->based0 != p.based0 || st->sx.size() < p.total) {
        g_c.seed_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    size_t at = st->history.size();
    for (size_t i = 0; i < st->history.size(); ++i) {
        if (st->history[i].dseq == p.dseq) {
            at = i;
            break;
        }
    }
    if (at == st->history.size()) {
        g_c.seed_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (uint32_t i = 0; i < p.n; ++i) {
        const uint32_t m = p.first + i;
        const uint8_t* g58 = p.got58.data() + 32 * size_t{i};
        st->sx[m] = LoadU32(p.gotd0.data(), 2 * size_t{i});
        st->sy[m] = LoadU32(p.gotd0.data(), 2 * size_t{i} + 1);
        st->sp[m] = LoadU32(g58, 6);
        st->known[m] = 1;
    }
    for (size_t h = at + 1; h < st->history.size(); ++h) {
        const DispatchRecord& r = st->history[h];
        const float dt = Float(r.dt_bits);
        const uint32_t end = p.first + p.n < r.count ? p.first + p.n : r.count;
        for (uint32_t m = p.first; m < end; ++m) {
            if (!r.ud_known || r.lod[m] == kNoLod) {
                st->known[m] = 0; // the transition of this dispatch is not known: wait for the next anchor
                continue;
            }
            if (!st->known[m] && r.kind != Kind::kSnap) {
                continue;
            }
            const StateRef ref{&st->sx[m], &st->sy[m], &st->sp[m]};
            if (r.kind == Kind::kSnap) {
                Snap(ref, r.lod[m]);
                st->known[m] = 1;
            } else {
                Advance(ref, r.lod[m], dt, r.ud10 > m);
            }
        }
    }
    // The records up to the anchor are no longer needed.
    for (size_t h = 0; h < at; ++h) {
        if (st->pool.size() < kPoolMax) {
            st->pool.push_back(std::move(st->history[h].lod));
        }
    }
    st->history.erase(st->history.begin(), st->history.begin() + static_cast<std::ptrdiff_t>(at));
    g_c.seeded.fetch_add(1, std::memory_order_relaxed);
}

// Verify optimizations added on top of the first mirror. All off: with them the mirror's agreement on the real GPU
// dropped (2 Nov runs), and none is proven harmless yet. Kept for the next round of the CPU cull work:
//   1 kOptWindow      a proven owner registers a 256-model prefix instead of the whole table
//   2 kOptSkipSeed    a compare that agrees does not re-anchor the window with the GPU copy
//   4 kOptInlineDrain compares run on the game's job threads (6 per dispatch, as the first mirror did) and not on the
//                     verify worker thread
constexpr uint32_t kOptWindow = 1, kOptSkipSeed = 2, kOptInlineDrain = 4;
uint32_t OptFlags() {
    return 0;
}

// Handles one pending compare whose GPU copies were read out (p.got58 / p.gotd0). No lock held on entry.
void Finish(PendingCheck& p, OwnerState* st) {
    std::lock_guard<std::shared_mutex> lock(st->mu);
    if (st->set != p.set || st->base58 != p.base58 || st->based0 != p.based0) {
        g_c.unverified.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t unknown_valid = 0;
    const bool agree = Compare(p, st, &unknown_valid);
    // A compare that agrees with every window model anchored leaves the carried state exactly where the dispatches put
    // it (the seed would rewrite the same values), so it is skipped. Otherwise the GPU truth re-anchors the window.
    if (agree && unknown_valid == 0 && (OptFlags() & kOptSkipSeed)) {
        g_c.seed_avoided.fetch_add(1, std::memory_order_relaxed);
        if (p.n == p.total) {
            st->need_full = false; // the whole table was verified and anchored
        }
    } else {
        Seed(p, st);
        if (!agree || unknown_valid != 0) {
            st->need_full = true;
        }
    }
}

// Reads out and compares the pending checks whose GPU copies are complete (oldest first), at most `limit`.
// Called by the worker, and by the report.
void Drain(size_t limit) {
    std::lock_guard<std::mutex> drain(g_drain_mutex);
    while (limit > 0) {
        --limit;
        PendingCheck p;
        OwnerState* st = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            while (!g_pending.empty() && g_pending.front().superseded) {
                g_pending.pop_front();
            }
            if (g_pending.empty()) {
                return;
            }
            p = std::move(g_pending.front());
            g_pending.pop_front();
            const auto it = g_owners.find(p.owner);
            st = it == g_owners.end() ? nullptr : &it->second;
        }
        p.got58.resize(p.bytes58); // allocated when the copies are read out, not while the check waits for the GPU
        p.gotd0.resize(p.bytesd0);
        bool unc58 = false;
        bool uncd0 = false;
        SotcNativeCopies::Miss miss58 = SotcNativeCopies::Miss::kNoKey;
        SotcNativeCopies::Miss missd0 = SotcNativeCopies::Miss::kNoKey;
        const uint64_t got58 =
            SotcNativeCopies::CopyOut(p.key58, p.frame, p.frame, p.bytes58, p.got58.data(), &unc58, &miss58);
        const uint64_t gotd0 =
            SotcNativeCopies::CopyOut(p.keyd0, p.frame, p.frame, p.bytesd0, p.gotd0.data(), &uncd0, &missd0);
        if (got58 == 0 || gotd0 == 0) {
            const bool waiting = miss58 == SotcNativeCopies::Miss::kFrame || missd0 == SotcNativeCopies::Miss::kFrame;
            if (waiting && SotcNativeCopies::CurrentFrame() - p.frame <= kMaxAgeFrames) {
                // The GPU has not finished this frame yet; the older entries are not ready either. Put it back (a
                // frame that can still be waited for takes no further registration, so it is not superseded).
                std::lock_guard<std::mutex> lock(g_mutex);
                // A newer dispatch of the owner in the same frame may have registered while p was out of the queue
                // (Enqueue could not mark it superseded). The copy of the frame is then the newer one's: comparing
                // p against it would corrupt the seed. Drop p.
                bool superseded = false;
                for (const PendingCheck& other : g_pending) {
                    if (other.owner == p.owner && other.frame == p.frame) {
                        superseded = true;
                        break;
                    }
                }
                if (!superseded) {
                    g_pending.push_front(std::move(p));
                }
                return;
            }
            g_c.unverified.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (st == nullptr) {
            g_c.unverified.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        Finish(p, st);
    }
}

void WorkerMain() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        const uint64_t t0 = NowNs();
        Drain(64);
        AddStageNs(Stage::kLodWorker, NowNs() - t0);
    }
}

void EnsureWorker() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread(WorkerMain).detach(); // lives for the process; it only touches leaked/static state
    });
}

// Queues the compare of a sampled dispatch (owner mutex held): registers the window of the two tables for the copy
// ring and snapshots what the worker needs (the mirror's state, the window's records). A registration the ring
// refuses leaves the dispatch unverified.
void Enqueue(uint64_t owner, const OwnerState& st, const DispatchRecord& rec, uint64_t pipe, const uint8_t* recs,
             uint32_t first, uint32_t n) {
    PendingCheck p;
    p.frame = rec.frame;
    p.dseq = rec.dseq;
    p.owner = owner;
    p.set = st.set;
    p.base58 = st.base58;
    p.based0 = st.based0;
    p.key58 = kKeyLod | (owner << 1);
    p.keyd0 = p.key58 | 1;
    p.total = rec.count;
    p.first = first;
    p.n = n;
    p.pipe = pipe;
    p.ud10 = rec.ud10;
    p.ud_known = rec.ud_known;
    p.kind = rec.kind;
    p.in = st.in;
    p.bytes58 = 32 * uint64_t{n};
    p.bytesd0 = 8 * uint64_t{n};
    if (!SotcNativeCopies::Register(p.key58, rec.frame, st.base58 + 32 * uint64_t{first}, p.bytes58, owner, true) ||
        !SotcNativeCopies::Register(p.keyd0, rec.frame, st.based0 + 8 * uint64_t{first}, p.bytesd0, owner, true)) {
        g_c.unverified.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p.expect.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t m = first + i;
        p.expect[i] = Row{st.sx[m], st.sy[m], st.sp[m], st.known[m]};
    }
    p.lod.assign(rec.lod.begin() + first, rec.lod.begin() + first + n);
    p.recs.assign(recs + kRecordBytes * size_t{first}, recs + kRecordBytes * size_t{first + n});
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (PendingCheck& other : g_pending) {
            if (other.owner == owner && other.frame == rec.frame) {
                other.superseded = true; // the newest registration of the key is this dispatch's copy
            }
        }
        if (g_pending.size() >= kMaxPending) {
            g_pending.pop_front();
            g_c.unverified.fetch_add(1, std::memory_order_relaxed);
        }
        g_pending.push_back(std::move(p));
    }
    g_c.sampled.fetch_add(1, std::memory_order_relaxed);
    if (!(OptFlags() & kOptInlineDrain)) {
        EnsureWorker();
    }
}

// ---- The dispatch ----------------------------------------------------------------------------------------------

struct StepEnv {
    const LodInputs* in;
    OwnerState* st;
    const uint8_t* recs;
    uint8_t* lod_out;
    bool modelled;
    bool snap;
    bool ud_known;
    uint32_t ud10;
    uint64_t poisoned = 0;
    uint32_t unknown_valid = 0;
};

// One model the general way: evaluate its LOD, then the carried state.
inline void StepOne(StepEnv& e, uint32_t m) {
    const uint8_t* rec = e.recs + kRecordBytes * size_t{m};
    uint32_t lod = 0;
    if (!EvalLod(*e.in, rec, &lod)) {
        e.st->known[m] = 0; // the GPU result is undefined or out of range here
        e.lod_out[m] = static_cast<uint8_t>(kNoLod);
        ++e.poisoned;
        return;
    }
    e.lod_out[m] = static_cast<uint8_t>(lod);
    OwnerState& st = *e.st;
    const StateRef ref{&st.sx[m], &st.sy[m], &st.sp[m]};
    if (!e.modelled) {
        st.known[m] = 0;
    } else if (e.snap) {
        Snap(ref, lod); // anchors the model: the variant reads no previous state
        st.known[m] = 1;
    } else if (st.known[m]) {
        Advance(ref, lod, e.in->dt, e.ud_known && e.ud10 > m);
    } else {
        ++e.unknown_valid;
    }
}

// Steps every model of the dispatch (see the header comment). The result is identical to calling StepOne for each.
void StepAll(StepEnv& e, uint32_t count) {
    uint64_t groups = 0;
    uint64_t skipped = 0;
    uint32_t m = 0;
    const LodInputs& in = *e.in;
    OwnerState& st = *e.st;
    if (in.lod_override < 0) {
        const __m128 one = _mm_set1_ps(1.0f);
        const bool dt_keeps_one = !(in.dt < 0.0f); // min(1, dt + 1) is 1 for dt >= 0 and for NaN
        for (; m + 4 <= count; m += 4) {
            uint32_t lods[4];
            if (!Lod4(in, e.recs, m, lods)) {
                for (uint32_t i = 0; i < 4; ++i) {
                    StepOne(e, m + i);
                }
                continue;
            }
            ++groups;
            const uint32_t packed = lods[0] | (lods[1] << 8) | (lods[2] << 16) | (lods[3] << 24);
            std::memcpy(e.lod_out + m, &packed, sizeof packed);
            if (e.modelled && !e.snap && e.ud_known && e.ud10 > m + 3 && dt_keeps_one) {
                uint32_t k4;
                std::memcpy(&k4, &st.known[m], sizeof k4);
                if (k4 == 0x01010101u) {
                    // The models whose LOD did not change and whose progress is saturated keep their state.
                    const __m128 x = _mm_loadu_ps(reinterpret_cast<const float*>(&st.sx[m]));
                    const __m128 p = _mm_loadu_ps(reinterpret_cast<const float*>(&st.sp[m]));
                    const __m128 lodf = _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lods)));
                    if (_mm_movemask_ps(_mm_cmpneq_ps(x, lodf)) == 0 && _mm_movemask_ps(_mm_cmpeq_ps(p, one)) == 0xF) {
                        ++skipped;
                        continue;
                    }
                }
            }
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t mi = m + i;
                const StateRef ref{&st.sx[mi], &st.sy[mi], &st.sp[mi]};
                if (!e.modelled) {
                    st.known[mi] = 0;
                } else if (e.snap) {
                    Snap(ref, lods[i]);
                    st.known[mi] = 1;
                } else if (st.known[mi]) {
                    Advance(ref, lods[i], in.dt, e.ud_known && e.ud10 > mi);
                } else {
                    ++e.unknown_valid;
                }
            }
        }
    }
    const uint32_t tail = count - m;
    for (; m < count; ++m) {
        StepOne(e, m);
    }
    g_c.simd_groups.fetch_add(groups, std::memory_order_relaxed);
    g_c.simd_skipped.fetch_add(skipped, std::memory_order_relaxed);
    g_c.scalar_models.fetch_add(count - 4 * groups, std::memory_order_relaxed);
    (void)tail;
}

// The part of a dispatch after its inputs are read: steps the mirror, registers the verify copies and records the
// dispatch. Owner mutex held exclusively. Separate from Dispatch so that an offline harness can drive it.
void RunDispatch(uint64_t owner, OwnerState& st, uint64_t frame, uint32_t count, const LodInputs& in, uint32_t dt_bits,
                 const uint8_t* recs, bool ud_known, uint32_t ud_value, Kind kind, uint64_t pipe, uint64_t t_start) {
    const bool modelled = ud_known && kind != Kind::kUnknown;
    const bool snap = kind == Kind::kSnap;
    DispatchRecord rec;
    rec.dseq = ++st.dseq;
    rec.frame = frame;
    rec.count = count;
    rec.dt_bits = dt_bits;
    rec.ud10 = ud_value;
    rec.ud_known = modelled;
    rec.kind = kind;
    if (!st.pool.empty()) {
        rec.lod = std::move(st.pool.back());
        st.pool.pop_back();
    }
    rec.lod.resize(count);

    StepEnv env{&in, &st, recs, rec.lod.data(), modelled, snap, ud_known, ud_value};
    StepAll(env, count);
    g_c.poisoned.fetch_add(env.poisoned, std::memory_order_relaxed);
    if (env.unknown_valid != 0) {
        st.need_full = true; // a stepped model is not anchored: the next sample covers the whole table
    }

    st.in = in;
    st.ud10 = ud_value;
    st.ud_known = modelled;
    st.snap = snap;
    st.count = count;
    st.frame = frame;
    g_c.models.fetch_add(count, std::memory_order_relaxed);
    AddStageNs(Stage::kLodStep, NowNs() - t_start);

    // Verify. EVERY dispatch is registered for the copy ring: the ring pairs the GPU writes of a base with the
    // registrations of that base in order, and the draw thread may still be recording writes of earlier frames, so a
    // skipped dispatch would hand its write to the next registration (observed: t58[7], d0 and p mismatches). All
    // registrations of an owner start at the same table base; only their length varies: the first kWindow models of a
    // proven owner, the whole table for an unproven one, when a stepped model is not anchored, and once every
    // kFullEvery frames. The length is decided at the owner's first dispatch of a native frame.
    if (st.sample_frame != frame) {
        const bool proven = st.streak >= kNeedStreak && st.proof_frame + kMaxProofAge >= frame;
        st.sample_frame = frame;
        if (!(OptFlags() & kOptWindow) || !proven || st.need_full || frame >= st.last_sample_frame + kFullEvery) {
            st.last_sample_frame = frame;
            st.sample_len = count;
        } else {
            st.sample_len = kWindow;
        }
    }
    {
        const uint64_t t_enq = NowNs();
        const uint32_t n = st.sample_len < count ? st.sample_len : count;
        Enqueue(owner, st, rec, pipe, recs, 0, n);
        AddStageNs(Stage::kLodEnqueue, NowNs() - t_enq);
    }

    st.history.push_back(std::move(rec));
    while (st.history.size() > kMaxHistory ||
           (!st.history.empty() && st.history.front().frame + kHistoryFrames < frame)) {
        if (st.pool.size() < kPoolMax) {
            st.pool.push_back(std::move(st.history.front().lod));
        }
        st.history.pop_front();
    }
}

// Steps every model of the owner for one pre-shader dispatch (hook NATIVE_LOD_DISPATCH). rbp is the frame of
// 0x19b400, ctx the command-list context (rbx at the site). Any failure drops the owner's carried state: the models
// become unknown and are anchored again by the next completed GPU copy.
void Dispatch(const GuestRegs& regs) {
    const uint64_t t_start = NowNs();
    const uint64_t owner = regs.r15;
    const uint64_t rbp = regs.rbp;
    const uint64_t ctx = regs.rbx;
    if (OptFlags() & kOptInlineDrain) {
        Drain(6); // before the owner lock: Finish takes owner locks
    }
    const uint64_t frame = SotcNativeCopies::CurrentFrame();
    OwnerState* const stp = FindOwner(owner, true);
    OwnerState& st = *stp;
    std::lock_guard<std::shared_mutex> lock(st.mu);
    st.frame = 0;
    g_c.dispatches.fetch_add(1, std::memory_order_relaxed);

    auto fail = [&st]() {
        st.set = 0;
        st.sx.clear();
        st.sy.clear();
        st.sp.clear();
        st.known.clear();
        st.history.clear();
        st.count = 0;
        st.streak = 0;
        g_c.failed.fetch_add(1, std::memory_order_relaxed);
    };

    uint32_t h[12] = {};
    if (!SotcGuest::Read(rbp + static_cast<uint64_t>(kHeaderOffset), h, sizeof h)) {
        return fail();
    }
    const uint32_t count = h[0];
    if (count == 0) {
        st.frame = frame; // nothing to step; the cull of this owner has no model
        st.count = 0;
        return;
    }
    if (count > kMaxModels) {
        return fail();
    }

    const uint64_t set = Ptr(owner + kOwnerSet);
    const uint64_t base58 = VBase(owner + kOwnerTable58V);
    const uint64_t based0 = VBase(owner + kOwnerTableD0V);
    const uint64_t basea8 = VBase(owner + kOwnerRecordsV);
    const uint64_t image = SotcHooks::ImageBase();
    // The global lives in the eboot's data segment, which is host memory, not a guest heap SotcGuest can read.
    uint64_t renderer = 0;
    if (image != 0) {
        std::memcpy(&renderer, reinterpret_cast<const void*>(image + kSceneGlobal), sizeof renderer);
    }
    const uint64_t scene = renderer ? VBase(renderer + kRendererSceneV) : 0;
    uint32_t s[kSceneDwords] = {};
    const uint8_t* recs =
        (set && base58 && based0 && basea8) ? SotcGuest::Backing(basea8, kRecordBytes * count) : nullptr;
    if (!set || !base58 || !based0 || !recs || !scene || !SotcGuest::Read(scene, s, sizeof s)) {
        static std::atomic<int> logged{0}; // diagnostic: why the inputs fail
        if (logged.fetch_add(1) < 12) {
            std::fprintf(stderr,
                         "SOTC CPUCULL LOD fail: owner %#llx count %u set %#llx base58 %#llx based0 %#llx basea8 %#llx "
                         "recs %p renderer %#llx scene %#llx ctx %#llx\n",
                         (unsigned long long)owner, count, (unsigned long long)set, (unsigned long long)base58,
                         (unsigned long long)based0, (unsigned long long)basea8, (const void*)recs,
                         (unsigned long long)renderer, (unsigned long long)scene, (unsigned long long)ctx);
        }
        return fail();
    }

    const uint64_t pipe = Ptr(ctx + kMaterialOffset + kMaterialDesc);
    if (st.pipe != 0 && st.pipe != pipe) {
        g_c.pipe_changed.fetch_add(1, std::memory_order_relaxed); // the variants alternate; both are modelled
    }
    if (st.set != set || st.base58 != base58 || st.based0 != based0) {
        st.set = set;
        st.base58 = base58;
        st.based0 = based0;
        st.sx.clear();
        st.sy.clear();
        st.sp.clear();
        st.known.clear();
        st.history.clear();
        st.streak = 0;
        st.need_full = true;
    }
    st.pipe = pipe;
    st.basea8 = basea8;
    if (st.sx.size() < count) {
        st.sx.resize(count, 0);
        st.sy.resize(count, 0);
        st.sp.resize(count, 0);
        st.known.resize(count, 0);
    }

    const bool want_text = g_ud_logs.load(std::memory_order_relaxed) < kMaxUdLogs;
    const UdResult ud = ResolveUd10(ctx, regs.r13, owner, rbp, renderer, want_text);
    if (want_text && g_ud_logs.fetch_add(1) < kMaxUdLogs) {
        std::fprintf(stderr, "SOTC CPUCULL LOD ud10: owner %#llx count %u pipe %#llx -> %s %u (%s)\n",
                     (unsigned long long)owner, count, (unsigned long long)pipe, ud.known ? "resolved" : "UNRESOLVED", ud.value, ud.how);
    }
    if (!ud.known) {
        g_c.ud_unresolved.fetch_add(1, std::memory_order_relaxed);
    }

    // The variant: the compute program bound by 0x85f220 (shader object [material + 0xc0a0], code address
    // (u32 at +8) << 8, as 0x85f25f hands it to the GPU) looked up in the draw thread's table of dispatched programs.
    Kind kind = Kind::kUnknown;
    uint64_t code = 0;
    uint64_t hash = 0;
    {
        const uint64_t shader = Ptr(ctx + kMaterialOffset + 0xc0a0);
        uint32_t lo = 0;
        if (shader != 0 && SotcGuest::Read(shader + 8, &lo)) {
            code = uint64_t{lo} << 8;
            hash = SotcNativeCopies::ProgramAt(code);
            kind = hash == kHashStateful ? Kind::kStateful : (hash == kHashSnap ? Kind::kSnap : Kind::kUnknown);
        }
    }
    g_c.kind_dispatches[static_cast<int>(kind)].fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> kind_logs{0};
    if (kind_logs.load(std::memory_order_relaxed) < 24 && kind_logs.fetch_add(1) < 24) {
        std::fprintf(stderr, "SOTC CPUCULL LOD variant: owner %#llx pipe %#llx code %#llx hash %#llx -> %s\n",
                     (unsigned long long)owner, (unsigned long long)pipe, (unsigned long long)code,
                     (unsigned long long)hash,
                     kind == Kind::kStateful ? "stateful" : (kind == Kind::kSnap ? "snap" : "UNKNOWN"));
    }

    LodInputs in{};
    in.dt = Float(h[1]);
    in.speed = Float(h[2]);
    in.d = Float(h[4]);
    in.b = Float(h[5]);
    in.c = Float(h[6]);
    in.e = Float(h[7]);
    in.a = Float(h[8]);
    in.f = Float(h[9]);
    in.cam[0] = Float(s[140]);
    in.cam[1] = Float(s[141]);
    in.cam[2] = Float(s[142]);
    in.scale = Float(s[147]);
    in.metric_override = Float(s[148]);
    in.lod_override = static_cast<int32_t>(s[151]);

    RunDispatch(owner, st, frame, count, in, h[1], recs, ud.known, ud.value, kind, pipe, t_start);
}

// sotc: SOTC_LOD_TRACE=1, diagnostic: the LOD pre-shader dispatches per 5 s by set model count (u16 at
// [owner+0x18]+0x64, the same set in every session) and distinct owners. Two sessions (e.g. with and without the
// opening cinematic) are compared by their model-count lists: the sets one of them stops dispatching.
struct OwnerTrace {
    const char* label;
    std::mutex mu;
    std::unordered_map<uint32_t, uint64_t> by_count;
    std::unordered_map<uint64_t, uint32_t> owners;
    uint64_t null_set = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    void Note(uint64_t owner) {
        uint64_t set = 0;
        uint16_t n = 0;
        const bool has_set = owner != 0 && SotcGuest::Read(owner + kOwnerSet, &set) && set != 0;
        if (has_set) {
            SotcGuest::Read(set + 0x64, &n);
        }
        std::lock_guard lock{mu};
        if (!has_set) {
            ++null_set;
        }
        ++by_count[n];
        owners[owner] = n;
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(5)) {
            return;
        }
        last = now;
        std::vector<std::pair<uint32_t, uint64_t>> rows(by_count.begin(), by_count.end());
        std::sort(rows.begin(), rows.end());
        std::string text;
        for (const auto& [count, calls] : rows) {
            text += " " + std::to_string(count) + ":" + std::to_string(calls);
        }
        std::fprintf(stderr, "SOTC %s (5 s): %zu owners, %llu calls with no set record; calls by set model count:%s\n",
                     label, owners.size(), static_cast<unsigned long long>(null_set), text.c_str());
        by_count.clear();
        owners.clear();
        null_set = 0;
    }
};

void TraceDispatch(const GuestRegs& regs) {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_LOD_TRACE");
        return v && v[0] == '1';
    }();
    if (!on) {
        return;
    }
    static OwnerTrace trace{"LOD TRACE"};
    trace.Note(regs.r15);
}

// sotc: SOTC_SETENTRY_PROBE (scripts/hooks.d/setentry_probe.py): every BPXPR_BuildSetEntry call, owner = rdi.
HookAction OnSetEntry(GuestRegs& regs, void*) {
    static OwnerTrace trace{"SETENTRY TRACE"};
    trace.Note(regs.rdi);
    return HookAction::Continue;
}

HookAction OnDispatch(GuestRegs& regs, void*) {
    TraceDispatch(regs);
    if (!SotcNativeMasks::Cpu() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    Dispatch(regs);
    return HookAction::Continue;
}

// The calling thread's memo of Get() results (one entry per model, valid for the stamp of the call).
struct MemoEntry {
    uint32_t stamp = 0;
    ModelBits bits{};
};
struct Memo {
    std::vector<MemoEntry> entries;
    uint32_t now = 0;
};
thread_local Memo t_memo;

} // namespace

OwnerLod::OwnerLod(uint64_t owner, uint32_t fade_slot) : slot_(fade_slot) {
    OwnerState* st = FindOwner(owner, false);
    if (st == nullptr) {
        return;
    }
    st->mu.lock_shared();
    const uint64_t frame = SotcNativeCopies::CurrentFrame();
    bool usable = false;
    if (st->frame == 0 || st->frame != frame || st->count == 0 || st->history.empty()) {
        g_c.refused_stale.fetch_add(1, std::memory_order_relaxed);
    } else if (st->streak < kNeedStreak || st->proof_frame + kMaxProofAge < frame) {
        // Served only while the mirror is proven: it predicted the GPU tables of the last completed frames.
        g_c.refused_unproven.fetch_add(1, std::memory_order_relaxed);
    } else if (fade_slot == 5 || fade_slot > 7) {
        g_c.refused_unknown.fetch_add(1, std::memory_order_relaxed); // never written by the pre-shader
    } else {
        records_ = SotcGuest::Backing(st->basea8, kRecordBytes * st->count);
        usable = records_ != nullptr;
    }
    if (!usable) {
        st->mu.unlock_shared();
        return;
    }
    state_ = st;
    Memo& memo = t_memo;
    if (memo.entries.size() < st->count) {
        memo.entries.resize(st->count);
    }
    if (++memo.now == 0) { // wrapped: no stamp may match the new generation
        for (MemoEntry& e : memo.entries) {
            e.stamp = 0;
        }
        memo.now = 1;
    }
    stamp_ = memo.now;
}

OwnerLod::~OwnerLod() {
    if (state_ != nullptr) {
        g_c.served_models.fetch_add(served_, std::memory_order_relaxed);
        static_cast<OwnerState*>(state_)->mu.unlock_shared();
    }
}

bool OwnerLod::Get(uint32_t model, ModelBits* out) {
    const OwnerState& st = *static_cast<const OwnerState*>(state_);
    if (model >= st.count) {
        return false;
    }
    MemoEntry& entry = t_memo.entries[model];
    if (entry.stamp == stamp_) {
        *out = entry.bits;
        return true;
    }
    // The carried state must be anchored, and the newest dispatch must have stepped the model.
    if (!st.known[model] || st.history.back().lod[model] == kNoLod) {
        g_c.refused_unknown.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint8_t* rec = records_ + kRecordBytes * size_t{model};
    const bool ud_ok = st.ud_known && st.ud10 > model;
    float fade;
    if (slot_ == 6) {
        fade = Float(st.sp[model]);
    } else if (slot_ == 7) {
        if (!(ud_ok || st.snap)) {
            g_c.refused_unknown.fetch_add(1, std::memory_order_relaxed); // the dispatch did not write it
            return false;
        }
        fade = Metric(st.in, rec);
    } else {
        if (slot_ == 4 && !ud_ok) {
            g_c.refused_unknown.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        fade = FadeWord(st.in, rec, static_cast<int>(slot_));
    }
    entry.bits.fade = fade;
    entry.bits.p = Float(st.sp[model]);
    entry.bits.d0[0] = st.sx[model];
    entry.bits.d0[1] = st.sy[model];
    entry.stamp = stamp_;
    ++served_;
    *out = entry.bits;
    return true;
}

void RegisterLod() {
    SotcHooks::Handler handler;
    handler.pre = &OnDispatch;
    handler.post = nullptr;
    handler.user = nullptr;
    SotcHooks::RegisterHandler(kLodHook, handler);
    SotcHooks::Handler entry;
    entry.pre = &OnSetEntry;
    entry.post = nullptr;
    entry.user = nullptr;
    SotcHooks::RegisterHandler("SETENTRY_PROBE", entry);
}

void DrainLodVerify(size_t limit) {
    Drain(limit);
}

void AddStageNs(Stage stage, uint64_t ns) {
    g_stage_ns[static_cast<int>(stage)].fetch_add(ns, std::memory_order_relaxed);
}

uint64_t TakeStageNs(Stage stage) {
    return g_stage_ns[static_cast<int>(stage)].exchange(0, std::memory_order_relaxed);
}

void TakeLodStats(LodStats& out) {
    // Diagnostic: the detail line of the LOD mirror and the shadow replay (every 5 s with the NATIVE_MASKS lines).
    if (g_c.dispatches.load(std::memory_order_relaxed) != 0) {
        const auto take = [](std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.exchange(0)); };
        std::fprintf(stderr,
                     "SOTC CPUCULL LOD detail: compares %llu (agree %llu, disagree %llu); state bad: d0[0] %llu, "
                     "d0[1] %llu, p %llu; fade bad %llu; seeded %llu (skipped %llu, avoided %llu); ud10 unresolved "
                     "%llu; pipe changed %llu; poisoned %llu; refused: unproven %llu, unknown-model %llu, stale %llu; "
                     "served %llu models; step: %llu SIMD groups (%llu skipped as unchanged), %llu scalar models\n",
                     take(g_c.compares), take(g_c.agree), take(g_c.disagree), take(g_c.bad_x), take(g_c.bad_y),
                     take(g_c.bad_p), take(g_c.bad_fade), take(g_c.seeded), take(g_c.seed_skipped),
                     take(g_c.seed_avoided), take(g_c.ud_unresolved), take(g_c.pipe_changed), take(g_c.poisoned),
                     take(g_c.refused_unproven), take(g_c.refused_unknown), take(g_c.refused_stale),
                     take(g_c.served_models), take(g_c.simd_groups), take(g_c.simd_skipped), take(g_c.scalar_models));
        {
            std::lock_guard<std::mutex> lock(g_pipe_mutex);
            for (int i = 0; i < 8; ++i) {
                const unsigned long long models = take(g_c.pipe_models[i]);
                const unsigned long long bad = take(g_c.pipe_bad[i]);
                if (models != 0) {
                    std::fprintf(stderr, "SOTC CPUCULL LOD pipe %#llx: models compared %llu, bad %llu\n",
                                 (unsigned long long)g_pipe_ids[i], models, bad);
                }
            }
        }
        {
            std::string words;
            char part[64];
            for (int k = 0; k < kWordKinds; ++k) {
                std::snprintf(part, sizeof part, " %s %llu", kWordName[k],
                              static_cast<unsigned long long>(g_c.word_bad[k].exchange(0)));
                words += part;
            }
            std::fprintf(stderr, "SOTC CPUCULL LOD words bad (models):%s; dispatches by variant: unknown %llu, stateful %llu, snap %llu\n",
                         words.c_str(), take(g_c.kind_dispatches[0]), take(g_c.kind_dispatches[1]),
                         take(g_c.kind_dispatches[2]));
        }
        ShadowDebugLine();
    }
    out.dispatches = g_c.dispatches.exchange(0, std::memory_order_relaxed);
    out.models = g_c.models.exchange(0, std::memory_order_relaxed);
    out.failed = g_c.failed.exchange(0, std::memory_order_relaxed);
    out.lod_mismatch = g_c.lod_mismatch.exchange(0, std::memory_order_relaxed);
    out.fade_mismatch = g_c.fade_mismatch.exchange(0, std::memory_order_relaxed);
    out.verified = g_c.verified.exchange(0, std::memory_order_relaxed);
    out.unverified = g_c.unverified.exchange(0, std::memory_order_relaxed);
    out.sampled = g_c.sampled.exchange(0, std::memory_order_relaxed);
}

} // namespace SotcCpuCull
