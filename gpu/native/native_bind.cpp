// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_bind implementation (Phase 2.4, see native_bind.h).
#include "native/native_bind.h"

#include "native/native_fill.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <xxhash.h>

#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace Native::Bind {

namespace {

// -- Material block offsets (docs/engine/native-render-map-bind.md §1; all proven) -------------
// Record pointers are contiguous by GNM ordinal: +0xc0a0 + 8*s (CS..LS = slots 0..6).
constexpr u64 kOffRecordBase = 0xc0a0;
// Layout-record pointers: +0xc180 + 8*s.
constexpr u64 kOffLayoutBase = 0xc180;
// Dirty bytes: three per-stage byte arrays at +0xc248 / +0xc250 / +0xc258.
constexpr u64 kOffChanged = 0xc248;
constexpr u64 kOffUdPending = 0xc250;
constexpr u64 kOffLayoutIdentical = 0xc258;
constexpr u64 kOffGroupPending = 0xc260; // == cb+0xc548 (draw kind alias)
constexpr u64 kOffPendingBits = 0xc264;  // u16 accumulator
constexpr u64 kOffModifierVs = 0xc168;   // staged per setter, NOT contiguous
constexpr u64 kOffModifierEs = 0xc170;
constexpr u64 kOffModifierLs = 0xc178;
constexpr u64 kOffTexSize = 0xc5e8;      // (hi16*width) * (lo16*height)

/// Per-stage record image lengths (see RecordImageWords in the header).
constexpr u32 kImageWords[kNumStages] = {
    7,  // CS (0): SetCsShaderWithModifier reads cs_regs[0..6] ([4..6] = COMPUTE_NUM_THREAD)
    12, // PS (1): SetPsShader350 reads ps_regs[0..11]
    7,  // VS (2): SetVsShader reads vs_regs[0..6]
    7,  // GS (3): SetGsShader reads gs_regs[0..6]
    4,  // ES (4): SetEsShader reads es_regs[0..3]
    7,  // HS (5): SetHsShader reads hs_regs[0..6]
    4,  // LS (6): SetLsShader reads ls_regs[0..3]
};

/// The generic flush path of 0x85fc10 reads layout-record fields up to +0x21 (the u16 tail at
/// +0x20; the staging families also consult +0x82/+0xca, gap N2-6). The identity hash window
/// stays inside the proven-read range; a wider window would hash neighbouring heap bytes whose
/// mutation would only destabilize the key (misses, never wrong hits — the layout pointer is
/// separately part of the key).
constexpr u64 kLayoutHashBytes = 0x24;

/// A record/layout pointer the guest body never dereferences (its flush path gates on the
/// changed bytes) can still hold the boot-time sentinel (0xffff...ffff) in never-staged slots.
/// The native derivation must not fault where the guest would not: an implausible pointer is
/// normalized to "stage absent" for the key — the same snapshot rule the fill side (N2-10)
/// applies on the CP thread, so both sides agree on the key.
inline bool PlausiblePtr(u64 p) {
    return p != 0 && p != ~0ull && p >= 0x10000 && p < 0x800000000000ull;
}

u32 ModifierOffsetForStage(u32 stage) {
    switch (stage) {
    case 2: return kOffModifierVs; // VS
    case 4: return kOffModifierEs; // ES
    case 6: return kOffModifierLs; // LS
    default: return 0;             // CS/PS/GS/HS are bound without a modifier
    }
}

u32 FoldModifier(u32 rsrc1, u32 modifier) {
    // The driver's own fold (gnmdriver.cpp:1586/1847: var = (rsrc1 & 0xfcfffc3f) | modifier;
    // CS uses the 0xfffffc3f mask — the raw modifier key field keeps both distinct either way).
    if (modifier == 0) {
        return rsrc1;
    }
    return (rsrc1 & 0xfcfffc3fu) | modifier;
}

u64 HashSpan(const void* data, u32 bytes) {
    return XXH3_64bits(data, bytes);
}

// -- The pipeline maps -------------------------------------------------------------------------
// Both maps are filled only from the fill side (N2-10); Lookup runs on the guest render threads
// (the hooks), Remember is expected on the command-processor side — hence the mutex.
std::mutex g_mutex;
std::unordered_map<Key128, const Vulkan::Pipeline*, Key128Hash> g_pipelines;
std::unordered_map<u64, const Vulkan::Pipeline*> g_fullscreen_programs;

std::atomic<u64> g_stats_lookups{0};
std::atomic<u64> g_stats_hits{0};
std::atomic<u64> g_stats_remembers{0};

/// The image-anchored draw-mode -> primitive table DAT_00f9f2e0 (0x94ec0:11 etc.). Image
/// offsets are read through Guest::ImageQword (gap N2-3 rule: never as host pointers).
constexpr VAddr kPrimitiveTableVa = 0xf9f2e0;

u32 ImageDword(VAddr va) {
    // ImageQword reads at the given offset; pick the containing qword and shift.
    const u64 qword = Guest::ImageQword(va & ~u64{7});
    return static_cast<u32>(qword >> ((va & 4) * 8));
}

bool ValidPrimitiveType(u32 value) {
    switch (static_cast<AmdGpu::PrimitiveType>(value)) {
    case AmdGpu::PrimitiveType::None:
    case AmdGpu::PrimitiveType::PointList:
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::LineStrip:
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::TriangleFan:
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::PatchPrimitive:
    case AmdGpu::PrimitiveType::AdjLineList:
    case AmdGpu::PrimitiveType::AdjLineStrip:
    case AmdGpu::PrimitiveType::AdjTriangleList:
    case AmdGpu::PrimitiveType::AdjTriangleStrip:
    case AmdGpu::PrimitiveType::RectList:
    case AmdGpu::PrimitiveType::LineLoop:
    case AmdGpu::PrimitiveType::QuadList:
    case AmdGpu::PrimitiveType::QuadStrip:
    case AmdGpu::PrimitiveType::Polygon:
        return true;
    default:
        return false;
    }
}

/// The decoded draw-kind set of 0x862510 (start-vertex SGPR slots) and the indirect draws
/// (0x10add0/0xdb620). Kinds beyond it are gap N2-5.
bool DrawKindGeometryRecord(u64 cb, u32 kind, u64* record, u32* stage, bool* low_byte) {
    // 0x862510.c:12-36: kind 0 -> cb+0xc398 byte +0x27 (VS, stage 2); 0x45/0xad/0x145/0x1ad ->
    // cb+0xc3b8 byte +0x19 (LS, stage 6); 0xb0 -> cb+0xc3a8 byte +0x19 (ES, stage 4).
    switch (kind) {
    case 0x0:
        *record = *reinterpret_cast<const u64*>(cb + 0xc398);
        *stage = 2;
        *low_byte = false; // byte +0x27
        return true;
    case 0x45:
    case 0xad:
    case 0x145:
    case 0x1ad:
        *record = *reinterpret_cast<const u64*>(cb + 0xc3b8);
        *stage = 6;
        *low_byte = true; // byte +0x19
        return true;
    case 0xb0:
        *record = *reinterpret_cast<const u64*>(cb + 0xc3a8);
        *stage = 4;
        *low_byte = true; // byte +0x19
        return true;
    default:
        return false;
    }
}

} // namespace

u32 RecordImageWords(u32 stage) {
    return stage < kNumStages ? kImageWords[stage] : 0;
}

const char* GateName(Gate gate) {
    switch (gate) {
    case Gate::None: return "ok";
    case Gate::NoDevice: return "no device context";
    case Gate::NoMaterial: return "material block unavailable";
    case Gate::RecordImageInvalid: return "record image invalid (dword1 != 0)";
    case Gate::NoLayoutRecord: return "no layout record for a staged stage";
    case Gate::PrimitiveUnknown: return "primitive type outside the GNM enum";
    case Gate::PassCacheIncomplete: return "fixed-function pass caches incomplete";
    case Gate::PipelineUnknown: return "pipeline unresolved (content key miss, N2-10)";
    }
    return "unknown";
}

bool DeriveSnapshot(u64 cb, MaterialSnapshot& out) {
    if (cb == 0) {
        return false;
    }
    out = {};
    out.cb = cb;
    out.material = cb + Guest::DeviceOff::MaterialBlock; // +0x2e8
    const u64 material = out.material;

    out.group_pending = *reinterpret_cast<const u32*>(material + kOffGroupPending);
    out.pending_bits = *reinterpret_cast<const u16*>(material + kOffPendingBits);
    std::memcpy(out.changed, reinterpret_cast<const u8*>(material + kOffChanged), kNumStages);
    std::memcpy(out.ud_pending, reinterpret_cast<const u8*>(material + kOffUdPending), kNumStages);
    std::memcpy(out.layout_identical,
                reinterpret_cast<const u8*>(material + kOffLayoutIdentical), kNumStages);

    for (u32 s = 0; s < kNumStages; ++s) {
        StageRecord& rec = out.stage[s];
        rec.record = *reinterpret_cast<const u64*>(material + kOffRecordBase + 8 * s);
        rec.layout = *reinterpret_cast<const u64*>(material + kOffLayoutBase + 8 * s);
        if (const u32 mod_off = ModifierOffsetForStage(s); mod_off != 0) {
            rec.modifier = *reinterpret_cast<const u32*>(material + mod_off);
        }
        if (!PlausiblePtr(rec.record)) {
            // Never-staged slot (boot sentinel) — the guest body never dereferences these, so
            // the snapshot must treat the stage as absent exactly like a null pointer.
            rec.record = 0;
            rec.layout = 0;
            continue;
        }
        if (!PlausiblePtr(rec.layout)) {
            rec.layout = 0;
        }
        rec.words = RecordImageWords(s);
        std::memcpy(rec.image, reinterpret_cast<const void*>(rec.record + 8),
                    rec.words * sizeof(u32));
    }

    // The texture-size constant (bind doc §2.3: (hi16*w) * (lo16*h)).
    const u32 tex_size = *reinterpret_cast<const u32*>(material + kOffTexSize);
    out.tex_size_product = ((tex_size >> 16) & 0xffffu) * (tex_size & 0xffffu);

    out.ok = true;
    return true;
}

Gate ValidateStage(const StageRecord& stage) {
    if (stage.record == 0) {
        return Gate::None; // absent is valid (the fullscreen path binds with a null PS record)
    }
    if (stage.words < 2 || stage.image[1] != 0) {
        // The guest driver rejects this ("Invalid shader address", gnmdriver.cpp:1841/1751).
        return Gate::RecordImageInvalid;
    }
    if (stage.layout == 0) {
        return Gate::NoLayoutRecord;
    }
    return Gate::None;
}

FixedFunctionState DeriveFixedFunction(u64 pass, u32 draw_mode, bool tess_prim) {
    FixedFunctionState ff{};
    if (pass == 0) {
        return ff;
    }
    // 0x94ec0:10-20 (and the 0x94d00/0x94dd0 copies): prim = (byte at pass+0x18 & 6) ? 9 :
    // DAT_00f9f2e0[mode]; the pass functions keep the cached VGT_PRIMITIVE_TYPE at +0x18.
    if (tess_prim) {
        ff.primitive_type = static_cast<u32>(AmdGpu::PrimitiveType::PatchPrimitive);
    } else {
        // Bound the draw mode the way the guest's table is sized (the table is read raw by the
        // guest too, but a mode beyond the enum cannot name a primitive — gate rather than read
        // out of bounds).
        if (draw_mode > 21) {
            return ff;
        }
        ff.primitive_type = ImageDword(kPrimitiveTableVa + 4 * draw_mode);
    }
    if (!ValidPrimitiveType(ff.primitive_type)) {
        return ff;
    }
    // The draw objects reset the INDEX_TYPE cache at pass+0x1c8 (0x94ec0:21-24: 0x86bed0(cb,0,2)
    // then cache = 0; 0x94d00/0x94dd0 leave it).
    ff.index_type = *reinterpret_cast<const u32*>(pass + Guest::PassOff::IndexTypeCache);
    // The fixed-function caches the pass functions maintain (bind doc §5,
    // native-render-map-pass.md §1.1). blend at +0x88 = param_1[0x11] (0x93960:31-33),
    // CB_TARGET_MASK at +0x1ac, DB_DEPTH_CONTROL at +0x11c, PA_SU_SC_MODE at +0x23.
    ff.cb_blend0 = *reinterpret_cast<const u32*>(pass + 0x88);
    ff.cb_target_mask = *reinterpret_cast<const u32*>(pass + 0x1ac);
    ff.db_depth_control = *reinterpret_cast<const u32*>(pass + 0x11c);
    ff.pa_su_sc_mode = *reinterpret_cast<const u32*>(pass + 0x23);
    ff.ok = true;
    return ff;
}

FixedFunctionState DeriveFixedFunctionCachedPrim(u64 pass) {
    FixedFunctionState ff{};
    if (pass == 0) {
        return ff;
    }
    ff.primitive_type = *reinterpret_cast<const u32*>(pass + Guest::PassOff::PrimitiveType);
    if (!ValidPrimitiveType(ff.primitive_type) || ff.primitive_type == 0) {
        return ff; // 0 = the cache was never set in this pass: unknown topology, chain.
    }
    ff.index_type = *reinterpret_cast<const u32*>(pass + Guest::PassOff::IndexTypeCache);
    ff.cb_blend0 = *reinterpret_cast<const u32*>(pass + 0x88);
    ff.cb_target_mask = *reinterpret_cast<const u32*>(pass + 0x1ac);
    ff.db_depth_control = *reinterpret_cast<const u32*>(pass + 0x11c);
    ff.pa_su_sc_mode = *reinterpret_cast<const u32*>(pass + 0x23);
    ff.ok = true;
    return ff;
}

MaterialKey BuildKey(const MaterialSnapshot& snap, const FixedFunctionState& ff) {
    MaterialKey key{};
    for (u32 s = 0; s < kNumStages; ++s) {
        const StageRecord& rec = snap.stage[s];
        StageKey& sk = key.stage[s];
        sk.present = rec.record != 0 ? 1u : 0u;
        if (sk.present == 0) {
            continue;
        }
        sk.layout_ptr = rec.layout;
        sk.layout_hash = HashSpan(reinterpret_cast<const void*>(rec.layout),
                                  static_cast<u32>(kLayoutHashBytes));
        sk.code_va = rec.image[0];
        sk.rsrc1 = FoldModifier(rec.image[2], rec.modifier);
        sk.rsrc2 = rec.words > 3 ? rec.image[3] : 0;
        sk.image_words = rec.words;
        sk.image_hash = static_cast<u32>(
            rec.words > 4
                ? HashSpan(rec.image + 4, (rec.words - 4) * sizeof(u32))
                : 0);
        sk.modifier = rec.modifier;
    }
    key.primitive_type = ff.primitive_type;
    key.index_type = ff.index_type;
    key.db_depth_control = ff.db_depth_control;
    key.cb_target_mask = ff.cb_target_mask;
    key.cb_blend0 = ff.cb_blend0;
    key.pa_su_sc_mode = ff.pa_su_sc_mode;
    key.draw_kind = snap.group_pending;
    key.version = kKeyVersion;
    return key;
}

Key128 HashKey(const MaterialKey& key) {
    const XXH128_hash_t h = XXH3_128bits(&key, sizeof(key));
    const Key128 digest{h.low64, h.high64};
    // sotc: the fill side (N2-10): the command processor builds the pipeline for this draw from
    // liverpool->regs and cannot name this key. Publishing the draw's shader signature with the
    // digest lets it find the pipeline again (Native::Bind::Fill).
    Fill::RegisterPending(Fill::SigFromKey(key), digest);
    return digest;
}

const Vulkan::Pipeline* Lookup(const Key128& key) {
    g_stats_lookups.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{g_mutex};
    const auto it = g_pipelines.find(key);
    if (it == g_pipelines.end()) {
        return nullptr;
    }
    g_stats_hits.fetch_add(1, std::memory_order_relaxed);
    return it->second;
}

void Remember(const Key128& key, const Vulkan::Pipeline* pipeline) {
    if (pipeline == nullptr) {
        return;
    }
    g_stats_remembers.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{g_mutex};
    g_pipelines.insert_or_assign(key, pipeline);
}

const Vulkan::Pipeline* LookupFullscreenProgram(u64 program) {
    g_stats_lookups.fetch_add(1, std::memory_order_relaxed);
    if (program == 0) {
        return nullptr;
    }
    std::scoped_lock lock{g_mutex};
    const auto it = g_fullscreen_programs.find(program);
    if (it == g_fullscreen_programs.end()) {
        return nullptr;
    }
    g_stats_hits.fetch_add(1, std::memory_order_relaxed);
    return it->second;
}

void RememberFullscreenProgram(u64 program, const Vulkan::Pipeline* pipeline) {
    if (program == 0 || pipeline == nullptr) {
        return;
    }
    g_stats_remembers.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{g_mutex};
    g_fullscreen_programs.insert_or_assign(program, pipeline);
}

Stats StatsSnapshot() {
    std::scoped_lock lock{g_mutex};
    Stats stats{};
    stats.lookups = g_stats_lookups.load(std::memory_order_relaxed);
    stats.hits = g_stats_hits.load(std::memory_order_relaxed);
    stats.remembers = g_stats_remembers.load(std::memory_order_relaxed);
    stats.distinct = g_pipelines.size() + g_fullscreen_programs.size();
    return stats;
}

bool DeriveStartVertexSlots(u64 cb, u32 kind, StartVertexSlots& out) {
    u64 record = 0;
    u32 stage = 0;
    bool low_byte = false;
    if (!DrawKindGeometryRecord(cb, kind, &record, &stage, &low_byte)) {
        return false;
    }
    if (record == 0) {
        return false;
    }
    const u32 raw = low_byte ? *reinterpret_cast<const u8*>(record + 0x19)
                             : *reinterpret_cast<const u8*>(record + 0x27);
    out.stage = stage;
    out.vertex_slot = raw & 0xfu;
    out.instance_slot = raw >> 4;
    return true;
}

} // namespace Native::Bind
