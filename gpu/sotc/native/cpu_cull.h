// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU culling of the XPR visibility masks (option B of docs/re-stutters/native/CPUCULL-BRIEF.md). The GPU cull
// writes its masks too late for the CPU consumer of the same frame, and a previous frame's mask enables the wrong
// LOD or fade instance for a frame (jitter). These functions compute the current frame's masks on the CPU from the
// same inputs the cull dispatch reads, at the producer site, so the consumer never waits for the GPU and never
// reads an older frame. Every function returns false when it cannot compute a mask it trusts; the caller then
// serves the last completed GPU copy (native_masks.cpp), as before.
//
// Masks are bit arrays, 1 bit per object in set order, 32 objects per dword (the layout the cull shader writes).
// Conservative rule: a CPU mask may set a bit the GPU would clear (it skips the Hi-Z occlusion test), never clear a
// bit the GPU would set.
#pragma once

#include <cstddef>
#include <cstdint>

namespace SotcCpuCull {

/// Per-model LOD and fade state of one frame, as the GPU instance pre-shader (eboot 0x19b400) leaves it in the owner's
/// tables (owner+0x58: 32 bytes per model, owner+0xd0: 8 bytes per model), reduced to what the culls read.
struct ModelBits {
    float fade;       ///< table58[fade_slot]
    float p;          ///< table58[6]: the fade progress
    uint32_t d0[2];   ///< the 8-byte record of owner+0xd0: the LOD word and the fade target
};

/// Read access to the CPU mirror of one owner for the duration of one cull call (cpu_cull_lod.cpp). The constructor
/// pins the owner (its mutex is held until the destructor, so no dispatch, verify or seed changes the state under the
/// cull) and checks that the mirror is usable now: a dispatch of the owner was seen in this native frame and the
/// mirror is proven against the GPU tables of the recent frames. Get() computes the fade word lazily from the
/// dispatch inputs (the same expressions the pre-shader replay uses) and memoizes it for the call; there is no lock,
/// hash lookup or allocation per model. Not thread safe: one instance per call, on the calling thread.
class OwnerLod {
public:
    /// `fade_slot` is the table58 word the cull reads (0..4, 6, 7). Slots the mirror cannot reproduce exactly (5, and
    /// 4 / 7 for a model the dispatch did not write) make Get() fail.
    OwnerLod(uint64_t owner, uint32_t fade_slot);
    ~OwnerLod();
    OwnerLod(const OwnerLod&) = delete;
    OwnerLod& operator=(const OwnerLod&) = delete;

    /// False: the mirror cannot serve this owner now; the caller must not use Get().
    bool ok() const { return state_ != nullptr; }

    /// The state of `model` after the owner's newest dispatch of this frame. False when the model is out of range or
    /// its carried state is not anchored on the GPU (the caller then fails the whole set, as before).
    bool Get(uint32_t model, ModelBits* out);

private:
    void* state_ = nullptr;           ///< the locked OwnerState (opaque)
    const uint8_t* records_ = nullptr; ///< the model records (64 bytes each) the dispatch read
    uint32_t slot_ = 0;
    uint32_t stamp_ = 0;
    uint64_t served_ = 0;
};

/// Hook NATIVE_LOD_DISPATCH (scripts/hooks.d/cpucull_lod.py, site 0x19b708 in 0x19b400, after the material binds and
/// before the pre-shader dispatch): steps the CPU mirror of every model of the owner. Registered by
/// SotcNativeMasks::Register (native_masks.cpp), active under SOTC_NATIVE_MASKS=prev.
void RegisterLod();

/// Where the CPU cull spends its time (SOTC NATIVE_MASKS cpu line, ms per second of wall time).
enum class Stage : uint8_t {
    kLodStep,    ///< the LOD mirror's dispatch hook on the game's job thread (without the verify enqueue)
    kLodEnqueue, ///< the verify enqueue of a sampled dispatch (job thread): copy registrations and snapshots
    kLodWorker,  ///< the verify worker (own host thread): GPU copy readout, compare, seed
    kMain,       ///< CullMain at the producer site
    kShadow,     ///< CullShadow at the producer site
    kServe,      ///< the consumer's Serve (served from the CPU mask)
    kVerify,     ///< the mask verify (SOTC_NATIVE=2, or SOTC_VERIFY in mode 1) on the consumer thread
    kCount
};
void AddStageNs(Stage stage, uint64_t ns);
uint64_t TakeStageNs(Stage stage);

/// Window counters of the LOD mirror (read and reset by the "SOTC NATIVE_MASKS cpu" line).
struct LodStats {
    uint64_t dispatches = 0;     ///< pre-shader dispatches stepped on the CPU
    uint64_t models = 0;         ///< models stepped
    uint64_t failed = 0;         ///< dispatches whose inputs could not be read (the owner is not served this frame)
    uint64_t lod_mismatch = 0;   ///< verify: models whose LOD words (table_d0) differ from the GPU copy
    uint64_t fade_mismatch = 0;  ///< verify: models whose fade words (table58) differ from the GPU copy (eps 1e-5)
    uint64_t verified = 0;       ///< verify: models compared
    uint64_t unverified = 0;     ///< verify: dispatches not compared (copy not registered, ring lost, or too late)
    uint64_t sampled = 0;        ///< dispatches whose GPU tables were registered for the verify
};
void TakeLodStats(LodStats& out);

/// Verify (SOTC_NATIVE=2): compares the pending dispatches whose GPU copies are complete (at most `limit` per call).
void DrainLodVerify(size_t limit);

/// Main scene (BPXPR_BuildSetDraws 0x19b760, site 0x19c0f6): the four masks of a set. `cull_block` is the 0x160-byte
/// constant block the dispatch reads, `owner` the set owner, `count` the object count (u16 at [owner+0x18]+0x64).
/// masks[i] has (count + 31) / 32 dwords. cpu_cull_main.cpp.
bool CullMain(uint64_t owner, uint64_t cull_block, uint32_t count, uint32_t* const masks[4]);

/// Shadow scene (sub_19c1a0, site 0x19c6b1): the two masks of one cascade or scene call. `index` is the shadow slot
/// index of the call. cpu_cull_shadow.cpp.
bool CullShadow(uint64_t owner, uint64_t cull_block, uint32_t index, uint32_t count, uint32_t* const masks[2]);

/// Decals (sub_19c7e0, site 0x19ce08): the mask of one decal owner. cpu_cull_decal.cpp.
bool CullDecal(uint64_t owner, uint64_t cull_block, uint32_t count, uint32_t* mask);

} // namespace SotcCpuCull
