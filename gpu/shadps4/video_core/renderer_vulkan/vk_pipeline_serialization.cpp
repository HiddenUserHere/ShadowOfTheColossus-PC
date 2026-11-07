// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_set>
#include "common/hash.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/info.h"
#include "video_core/sotc_stats.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "../../../sotc/predict_probe.h" // sotc: SOTC_PREDICT_PROBE
#include "../../../sotc/block_ledger.h" // sotc: SOTC_BLOCK_LEDGER
#include "../../../sotc/nonblocking.h"  // sotc: SOTC_NB_LAZY_PSO
#include "video_core/sotc_arch/gcn_native.h" // sotc: ARCH-GCN-NATIVE (ShaderBinaryVersion, profile bytes)

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// Above both lines before the upstream merge: generation changed, so old caches are dropped.
// sotc: 33 = the draw thread merge (SRT walker faults read zero instead of patching). 34 = the
// LOD feedback (sotc_lod_stats.h); binaries built with SOTC_LOD_FEEDBACK=0 are another version.
// 37/135 = invariant position on barycentric-emulation pipelines (ground-level texture/depth
// mismatch between the pipeline with the geometry shader and the one without; PR #258).
// 38/136 = SRT walker resolves Phis whose operands compute the same offset (flattened layout
// changes for those shaders).
// sotc: 44/141 (from 43/140): PersistentSrtInfo holds the SRT walk plan (SOTC_CP_SRT_PLAN, srt.h); the record is longer.
// sotc: SOTC_STAGE_BINDINGS=1 binaries number their descriptors per stage: +1000 keeps them apart
// from the others (the layouts and the shader bindings differ).
// sotc: SOTC_BINDLESS_SAMP=1 binaries read their samplers from the heap: +2000 keeps them apart.
static bool BindlessSamplersRequested() {
    const char* v = std::getenv("SOTC_BINDLESS_SAMP");
    return v && v[0] == '1';
}
// sotc: SOTC_SRT_PHI=0 flattens Phi shaders the old way: +4000 so cached walkers of the other
// setting are not reused (the A/B would be void otherwise).
static bool SrtPhiDisabled() {
    const char* v = std::getenv("SOTC_SRT_PHI");
    return v && v[0] == '0';
}
// sotc: SOTC_ADDR_VERTEX=1 or 2 (stage 3 of ARCH-BINDSET) gets +8000 so its binaries are kept apart. Its vertex pulling
// changes the shaders' output; the first step (indirect and index buffers through the table) does not, so the term
// only separates the caches until then.
static bool AddrVertexRequested() {
    const char* v = std::getenv("SOTC_ADDR_VERTEX");
    return v && (v[0] == '1' || v[0] == '2');
}
// sotc: table-mode shader addressing (SOTC_ADDR_TABLE and SOTC_SHADER_ADDR both on, as addr_table.cpp TableAsked)
// gets +1024000: its loads became valid per element (BdaLoadTable, 4 Nov 2026), so the binaries cached before
// that read zero for a whole record that ends past its buffer. Other settings keep their caches.
static bool AddrTableRequested() {
    const auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && (v[0] == '1' || v[0] == '2'); // SotcArch::SwitchMode != 0
    };
    return on("SOTC_ADDR_TABLE") && on("SOTC_SHADER_ADDR");
}
// sotc: SOTC_VS_BINDING_BASE (+64000): the vertex shaders' binding numbers start at a fixed base.
// sotc: SOTC_GENERIC_BUFFERS=1 (+16000) and =2 (+32000 more): the keys of generic buffers hold stride 0 and the generic
// format marker, so the binaries of another setting would be reused for the wrong buffers.
// sotc: 39/137 (from 38/136): the SRT walker no longer uses dynamic-window ReadConsts as offsets.
// 40/138: the cache now takes loose files newer than the pack (cache_storage.cpp), which brought
// back binaries written by older recompiler builds under the same number (wrong vertices, dark
// trees): everything is translated again once.
// 42/139 (4 Nov 2026): SOTC_SPV_FUNCS on by default, the generic buffer decode is a DontInline function.
// 43/140 (4 Nov 2026): no program hash in the SPIR-V (SOTC_SPV_PROGRAM_IDS, info.h): equal code is equal bytes, so the
// driver's pipeline cache shares the pipelines of programs that translate to the same shader.
// 41 (from 40): early fragment tests on the LOD feedback shaders (emit_spirv.cpp, upstream
// 0.70's 38); the non-feedback binaries are unchanged.
// sotc: SOTC_PRETRANSLATE / SOTC_PRECOMPILE (+128000): those caches held permutations whose resources differed from
// their program's Info (a layout variant without the flat buffer, then draws' translations with it, recorded under
// the variant's Info): the device lost of 4 Nov. Their caches are made again; the others are unchanged.
static const u32 ShaderBinaryVersion = (Shader::LodFeedbackEnabled() ? 44u : 141u) +
                                       (Shader::StageBindingsEnabled() ? 1000u : 0u) +
                                       (BindlessSamplersRequested() ? 2000u : 0u) +
                                       (SrtPhiDisabled() ? 4000u : 0u) +
                                       (AddrVertexRequested() ? 8000u : 0u) +
                                       (Shader::GenericBufferStride() ? 16000u : 0u) +
                                       (Shader::GenericBufferFormat() ? 32000u : 0u) +
                                       (Shader::Backend::VsBindingBaseOn() ? 64000u : 0u) +
                                       (SotcPretranslate::PixelVertexOn() ? 128000u : 0u) + // sotc: compute-only keeps the cache
                                       // sotc: ARCH-GCN-NATIVE: the gcn buffer and texture profiles change the SPIR-V
                                       // (profile.h). Zero terms while both stages are off: today's caches stay valid.
                                       // Mode, not ProfileValue: this runs at static init, and ProfileValue may refuse.
                                       (SotcGcn::Mode(SotcGcn::Stage::Buf) != 0 ? 256000u : 0u) +
                                       (SotcGcn::Mode(SotcGcn::Stage::Tex) != 0 ? 512000u : 0u) +
                                       (AddrTableRequested() ? 1024000u : 0u) +
                                       // sotc: SOTC_SPV_SALT (info.h) and SOTC_GCN_MISS_EMIT=0: diagnostic caches apart.
                                       Shader::SpvSalt() * 4096000u + (Shader::GcnMissEmit() ? 0u : 2048000u) +
                                       // sotc: SOTC_SPV_FUNCS (on by default): the generic buffer decode and the table
                                       // loads are DontInline functions (first-run compile cost); 0 keeps them inline.
                                       (Shader::SpvFuncs() ? 0u : 512u) +
                                       // sotc: SOTC_GCN_BUF_FMT=0 (info.h): formatted buffers kept on descriptors, apart.
                                       (Shader::GcnBufFormatted() ? 0u : 0x40000000u) +
                                       // sotc: SOTC_SPV_PROGRAM_IDS=1 (info.h): modules with the program hash, apart.
                                       (Shader::SpvProgramIds() ? 256u : 0u) +
                                       // sotc: SOTC_VS_DRAW_KIND=1 (info.h): instanced vertex modules shared by both
                                       // draw kinds (user data + BaseInstance), apart.
                                       (Shader::VsDrawKindOn() ? 128u : 0u);
// sotc: 22 (from 21): the record ends with the stage variant (SOTC_HEAP). Without it a cached Plain or Heap
// program came back as Default, so its layout disagreed with its binary (slot-word assert, device lost).
// 23: a variant's pgm_hash is the game's hash again (Info::CacheHash keys the caches), so 22's variant records differ.
// 24: BufferResource::sotc_read_extent (SOTC_SHADER_EXTENT) makes the buffer records 4 bytes longer; cached programs
// are translated once again (the SPIR-V itself is unchanged).
// 25: the record ends with has_discard and has_storage_images. They are Info members outside InfoPersistent, so every
// cached shader came back with both false: SOTC_NO_PREPASS=3 took alpha-tested pre-pass draws for opaque ones and
// dropped them (grass without alpha, 3 Nov), and the host's storage-image checks (op writes, fast bind, bind cache)
// missed cached shaders.
// 26: worker-translated variants (SOTC_HEAP, SOTC_SHADER_ADDR) lost ShaderParams::guest_hash, so their records hold
// the variant-mixed hash as pgm_hash: the binary was never found at load, the game hash rules missed the program, and
// the slot check kept the broken record against every rewrite. A cache folder without variants (neither switch)
// still accepts 25, whose records are fine there (MetaVersionAccepted).
static constexpr u32 ShaderMetaVersion = 26u;
static constexpr u32 ShaderMetaVersionNoVariants = 25u;
/// sotc: set by WarmUp from the profile before the cache is read: this folder's programs can be variants.
static bool g_variant_folder = false;
static bool MetaVersionAccepted(u32 version) {
    return version == ShaderMetaVersion || (version == ShaderMetaVersionNoVariants && !g_variant_folder);
}
// sotc: SOTC_VS_BINDING_BASE (+4000): the vertex stage's binding numbers start at a fixed base, so the pipelines (and
// their layouts) of another setting are not the same.
static const u32 PipelineKeyVersion =
    3u + (Shader::StageBindingsEnabled() ? 1000u : 0u) + (BindlessSamplersRequested() ? 2000u : 0u) +
    (Shader::Backend::VsBindingBaseOn() ? 4000u : 0u);
} // namespace Serialization

namespace Vulkan {

// sotc: SOTC CACHE DIAG counters (defined before LoadGraphicsPipeline; also declared in vk_pipeline_cache.cpp).
constexpr int kCacheDiagCount = 10;
extern std::atomic<u32> g_cache_diag[kCacheDiagCount];
void CacheDiagReport(const char* when);

namespace {
// sotc: where the boot-time preload spends its time (logged with "Preloaded N pipelines").
struct WarmUpTimes {
    std::chrono::steady_clock::duration load{}, modules{}, pipelines{};
} warmup_times;

struct WarmUpTimer {
    std::chrono::steady_clock::duration& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WarmUpTimer() {
        total += std::chrono::steady_clock::now() - start;
    }
};

long long Ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

// sotc: a shader permutation is stored under its index in the program ({pgm}_{idx}.spv, and a
// meta named after a hash of the same pair). The index is the order in which this session met the
// permutations, so a session that did not preload all of them can give an index already used on
// disk by another permutation. Writing over it paired the pipelines saved earlier with the wrong
// shader: the driver refused them at the next start (ErrorUnknown on AMD). Such a permutation is
// now left out of the cache, along with every pipeline that uses it; they compile each session.
std::mutex collided_mutex;
std::unordered_set<u64> collided_bins;      // BinaryId of the permutations left out
std::unordered_set<size_t> collided_perms; // perm hashes, as in the pipeline keys

u64 BinaryId(u64 pgm_hash, size_t perm_idx) {
    return pgm_hash ^ (u64(perm_idx) << 56);
}

bool IsCollidedPerm(size_t perm_hash) {
    std::scoped_lock lock{collided_mutex};
    return collided_perms.contains(perm_hash);
}

// sotc: whether the meta of this slot ({pgm}_{idx}) is one this build loads. A binary whose slot
// has none (no meta, or one of an older version) belongs to no pipeline any more: an older build
// left it. Before, it still counted as taken, so the permutation was left out of the cache and the
// slot's meta was never rewritten: the shader was translated again every session, its pipelines
// counted as stale at every start.
} // namespace
namespace {
// sotc: a stage of the SOTC_PRETRANSLATE pack by its stage hash (HashCombine(program hash, permutation)), for the
// readers that find no loose file (the pack loads no longer write loose copies). Defined with the pack below.
bool PackStageMeta(u64 stage_hash, std::vector<u8>& meta);
bool PackStageSpv(u64 stage_hash, std::vector<u32>& spv);

bool SlotHasCurrentMeta(u64 pgm_hash, size_t perm_idx) {
    std::vector<u8> blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", HashCombine(pgm_hash, u64(perm_idx))),
                                       blob);
    if (blob.empty()) {
        PackStageMeta(HashCombine(pgm_hash, u64(perm_idx)), blob); // a pack slot is taken too
    }
    if (blob.empty()) {
        return false;
    }
    try {
        Serialization::Archive ar{std::move(blob)};
        Serialization::Reader meta{ar};
        u32 meta_version{};
        u32 binary_version{};
        meta.Read(meta_version);
        meta.Read(binary_version);
        return Serialization::MetaVersionAccepted(meta_version) &&
               binary_version == Serialization::ShaderBinaryVersion;
    } catch (const Serialization::CorruptedData&) {
        return false;
    }
}

} // namespace

namespace {
// sotc: N5 use count. A pipeline key blob may end with a trailer: kUseCountMagic and the use count
// (8 bytes), written only with N5 on. A blob without it (an older cache, or N5 off) reads as count
// 0, and the loaders stop at the end of the pipeline data, so both kinds load under either setting.
constexpr u32 kUseCountMagic = 0x4E355543; // 'CU5N' in memory order
constexpr size_t kUseCountTrailer = 2 * sizeof(u32);

bool HasUseCountTrailer(const std::vector<u8>& blob) {
    if (blob.size() < kUseCountTrailer) {
        return false;
    }
    u32 magic{};
    std::memcpy(&magic, blob.data() + blob.size() - kUseCountTrailer, sizeof(u32));
    return magic == kUseCountMagic;
}

void WriteUseCount(Serialization::Writer& pldata, u32 uses) {
    if (!N5PsoEnabled()) {
        return;
    }
    pldata.Write(kUseCountMagic);
    pldata.Write(uses);
}
} // namespace

u32 PipelineUseCount(const std::vector<u8>& blob) {
    if (!HasUseCountTrailer(blob)) {
        return 0;
    }
    u32 uses{};
    std::memcpy(&uses, blob.data() + blob.size() - sizeof(u32), sizeof(u32));
    return uses;
}

bool RewritePipelineUseCount(const std::string& name, u32 uses) {
    std::vector<u8> blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::PipelineKey, name, blob);
    if (blob.empty()) {
        return false;
    }
    if (HasUseCountTrailer(blob)) {
        blob.resize(blob.size() - kUseCountTrailer);
    }
    const u32 trailer[2] = {kUseCountMagic, uses};
    const auto* bytes = reinterpret_cast<const u8*>(trailer);
    blob.insert(blob.end(), bytes, bytes + sizeof(trailer));
    return Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey, name,
                                              std::move(blob));
}

// sotc: returns false when nothing was queued for saving (not opened, or a collided permutation).
bool RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened() || IsCollidedPerm(key.value)) {
        return false;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);
    WriteUseCount(pldata, 1);

    return Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                              fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

// sotc: returns false when nothing was queued for saving (not opened, or a collided permutation).
bool RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return false;
    }
    for (const auto stage_hash : key.stage_hashes) {
        if (stage_hash && IsCollidedPerm(stage_hash)) {
            return false;
        }
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);
    WriteUseCount(pldata, 1);

    return Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                              fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

// sotc: the shader cache index (PipelineCache::SaveShaderIndex): every program with metas on disk and its permutation
// count, so a session finds the programs without reading them (PipelineCache::LoadLazyProgram).
std::mutex g_index_mutex;
std::unordered_map<u64, u32> g_index;
bool g_index_dirty = false;
constexpr const char* kShaderIndexName = "sotc_shader_index";

u32 SotcShaderMetaVersion() {
    return Serialization::ShaderMetaVersion;
}

// sotc: the bytes of a shader meta (RegisterShaderMeta's record; also what the SOTC_PRETRANSLATE pack stores).
std::vector<u8> SerializeShaderMeta(const Shader::Info& info, const Shader::StageSpecialization& spec, size_t perm_hash,
                                    size_t perm_idx) {
    Serialization::Archive ar;
    Serialization::Writer meta{ar};
    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);
    meta.Write(perm_hash);
    meta.Write(perm_idx);
    spec.Serialize(ar);
    info.Serialize(ar);
    return ar.TakeOff();
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    SotcPredict::OnTranslate(info, spec, perm_idx); // sotc: SOTC_PREDICT_PROBE (gpu/sotc/predict_probe.cpp)
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    {
        std::scoped_lock lock{collided_mutex};
        if (collided_bins.contains(BinaryId(info.CacheHash(), perm_idx))) {
            collided_perms.insert(perm_hash);
            return;
        }
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta, fmt::format("{:#018x}", perm_hash),
                                       SerializeShaderMeta(info, spec, perm_hash, perm_idx));
    {
        std::scoped_lock lock{g_index_mutex};
        auto& count = g_index[info.CacheHash()];
        if (perm_idx + 1 > count) {
            count = static_cast<u32>(perm_idx + 1);
            g_index_dirty = true;
        }
    }
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::RegisterBinary};

    const auto name = fmt::format("{:#018x}_{}", pgm_hash, perm_idx);
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        std::vector<u32> stored;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary, name, stored);
        if (!stored.empty() && stored != spv && !SlotHasCurrentMeta(pgm_hash, perm_idx)) {
            // An older build's binary: replaced (the meta is written right after).
            SotcStats::shader_cache_replaced.fetch_add(1, std::memory_order_relaxed);
        } else if (!stored.empty() && stored != spv) {
            SotcStats::shader_cache_left_out.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render_Vulkan,
                            "Shader permutation {} is another one on disk: not cached", name);
            }
            std::scoped_lock lock{collided_mutex};
            collided_bins.insert(BinaryId(pgm_hash, perm_idx));
            return;
        }
    }
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary, name, std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (!Serialization::MetaVersionAccepted(meta_version)) {
        g_cache_diag[1].fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        g_cache_diag[1].fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    if (!info.Deserialize(ar)) {
        g_cache_diag[2].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    // Stages left over from a pipeline whose load failed half-way must not leak into this one:
    // a stale entry for a stage this key does not use would be bound without user data.
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        PackStageMeta(compute_key.value, meta_blob); // sotc: a stage of the pretranslate pack
    }
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    {
        WarmUpTimer timer{warmup_times.pipelines};
        // sotc: built by the pipeline workers (SOTC_ASYNC_PRELOAD); WarmUp waits for them.
        const bool deferred = AsyncPreloadEnabled();
        // sotc: SOTC_ADDR_FALLBACK: the profile its stage was translated with (StageProfile).
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, layout_cache,
                                                       StageProfile(infos[0]->variant),
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, true, deferred);
        it.value()->use_count = warm_uses;
        it.value()->saved_use_count = warm_uses;
        if (deferred) {
            auto job = std::make_unique<AsyncPipelineJob>();
            job->compute = it.value().get();
            {
                std::scoped_lock lock{async_mutex};
                PushJobLocked(JobLane::Compute, std::move(job));
            }
            async_cv.notify_one();
            preload_computes.push_back(compute_key);
        }
    }

    infos.fill(nullptr);
    modules.fill(nullptr);

    if (AsyncPreloadEnabled()) {
        return true; // checked by WarmUp once built
    }
    if (!it.value()->Handle()) {
        compute_pipelines.erase(compute_key);
        return false;
    }
    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
    if (with_gs) {
        sdata.Write(gs);
    }
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    if (with_gs) {
        sdata.Read(gs);
    }
    return true;
}

// sotc: SOTC CACHE DIAG lines (always on). Why warm-up pipelines are stale, and what the game compiles at runtime (counted in
// vk_pipeline_cache.cpp). One line at the end of the warm-up, one every 128 runtime compiles.
std::atomic<u32> g_cache_diag[kCacheDiagCount]{};
void CacheDiagReport(const char* when) {
    std::fprintf(stderr,
                 "SOTC CACHE DIAG %s: stale: meta missing %u, meta version %u, meta info %u, spv missing %u, "
                 "perm conflict %u, no handle %u; runtime: new programs %u, new perms %u "
                 "(async %u)\n",
                 when, g_cache_diag[0].load(), g_cache_diag[1].load(), g_cache_diag[2].load(),
                 g_cache_diag[3].load(), g_cache_diag[4].load(), g_cache_diag[5].load(), g_cache_diag[6].load(),
                 g_cache_diag[8].load(), g_cache_diag[9].load());
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    // Stages left over from a pipeline whose load failed half-way must not leak into this one:
    // a stale entry for a stage this key does not use would be bound without user data.
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;
    graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            PackStageMeta(hash, meta_blob); // sotc: a stage of the pretranslate pack
        }
        if (meta_blob.empty()) {
            g_cache_diag[0].fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    ASSERT(is_new);

    // sotc: built by a pipeline worker; the map entry stays null (draws skipped, full-screen
    // passes built on the command processor) until the command processor collects it
    // (SOTC_ASYNC_PRELOAD). Not the ones whose construction reads per-draw state.
    const bool async = AsyncPreloadEnabled() && AsyncPipelineEligible();
    if (async) {
        QueueAsyncPipeline(std::hash<GraphicsPipelineKey>{}(graphics_key), &sdata, false);
    } else {
        WarmUpTimer timer{warmup_times.pipelines};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, layout_cache, StagesProfile(infos), graphics_key, *pipeline_cache, infos,
            runtime_infos, fetch_shader, modules, sdata, true);
    }

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    if (!async && it.value()) {
        it.value()->use_count = warm_uses;
        it.value()->saved_use_count = warm_uses;
    }
    if (async) {
        return true; // a refused one is dropped when collected (CollectAsyncPipelines)
    }
    if (!it.value()->Handle()) {
        g_cache_diag[5].fetch_add(1, std::memory_order_relaxed);
        graphics_pipelines.erase(graphics_key);
        return false;
    }
    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx)) {
        return false; // counted by LoadShaderMeta (SOTC CACHE DIAG)
    }
    SotcPredict::OnCachedProgram(program->info, spec); // sotc: SOTC_PREDICT_PROBE (pixel layouts per pass)

    std::vector<u32> spv{};
    {
        WarmUpTimer timer{warmup_times.load};
        Storage::DataBase::Instance().Load(
            Storage::BlobType::ShaderBinary,
            fmt::format("{:#018x}_{}", program->info.CacheHash(), perm_idx), spv);
        if (spv.empty()) { // sotc: a stage of the pretranslate pack
            PackStageSpv(HashCombine(program->info.CacheHash(), u64(perm_idx)), spv);
        }
    }
    if (spv.empty()) {
        if (g_cache_diag[3].fetch_add(1, std::memory_order_relaxed) < 6) {
            std::fprintf(stderr, "SOTC CACHE DIAG spv missing: %#018llx_%zu (pgm %#018llx, variant %u)\n",
                         static_cast<unsigned long long>(program->info.CacheHash()), perm_idx,
                         static_cast<unsigned long long>(program->info.pgm_hash), unsigned{program->info.variant});
        }
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    // Shader patches (user/shader/patch) must also apply to pipelines restored from the cache.
    if (EmulatorSettings.IsPatchShaders()) {
        if (auto patch = GetShaderPatch(program->info.pgm_hash, program->info.hw_stage, perm_idx,
                                        "spv", spv)) {
            LOG_INFO(Loader, "Loaded patch for cached {} shader {:#x}", program->info.hw_stage,
                     program->info.pgm_hash);
            spv = std::move(*patch);
        }
    }

    // sotc: keyed like GetProgram (ShaderParams::hash): a variant has its own entry under the game's pgm_hash.
    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.CacheHash());
    if (new_program) {
        {
            WarmUpTimer timer{warmup_times.modules};
            module = CompileSPV(spv, instance.GetDevice());
        }
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                g_cache_diag[4].fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            module = it->module;
        } else {
            WarmUpTimer timer{warmup_times.modules};
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    // sotc: a permutation added to a program that exists: its bindless answers must agree with the program's.
    if (!new_program) {
        Shader::CombineBindlessCache(it_pgm.value()->info.bindless, program->info.bindless);
    }
    // The specialization must reference the cached program's info, not the temporary one.
    spec.info = &it_pgm.value()->info;
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    infos[stage] = &it_pgm.value()->info;
    modules[stage] = module;
    if (auto& fetch = it_pgm.value()->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }

    return true;
}

void PipelineCache::WarmUp() {
    // sotc: SOTC_PIPE_MANIFEST loads at the first lookup after this (every return path).
    struct WarmUpDone {
        std::atomic<bool>& done;
        ~WarmUpDone() {
            done.store(true, std::memory_order_release);
        }
    } warmup_done_scope{warmup_done};
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }
    // sotc: N5. The compiles of the warm-up (and its boot prebuild) are not stalls of the game.
    struct WarmingScope {
        WarmingScope() {
            N5SetWarming(true);
        }
        ~WarmingScope() {
            N5SetWarming(false);
        }
    } warming_scope;

    // sotc: the variant key of the shader cache folder (DataBase::SetVariant): the binary version (its env terms),
    // the profile's switch-driven members, and the switches that change the SPIR-V without being in either.
    {
        std::string key = fmt::format("bin{} lds{} gs{} stage{} bda{} bindless{} heap{} addr{}",
                                      Serialization::ShaderBinaryVersion, profile.needs_lds_barriers,
                                      profile.barycentric_via_gs, profile.stage_bindings, profile.bda_buffers,
                                      profile.bindless_images, profile.heap_images, profile.shader_addr);
        // sotc: SOTC_ADDR_FALLBACK: a table-mode folder of its own. It holds both kinds of programs (the table variants
        // and the descriptor-mode default variants, each found by its hash), and none of the default-variant programs
        // an older table run may have translated with the table profile (SOTC_ASYNC_COMPUTE_XLATE prefetch).
        if (addr_fallback) {
            key += " addrfb1";
        }
        for (const char* name : {"SOTC_WQM_COPY", "SOTC_WAVE64_UNIFORM", "SOTC_WAVE64_IDENTITY",
                                 "SOTC_TSHARP_ARRAY", "SHADPS4_IEEE_MINMAX"}) {
            if (const char* v = std::getenv(name); v && *v) {
                key += fmt::format(" {}={}", name, v);
            }
        }
        cache_variant_key = key; // sotc: SOTC_PIPE_MANIFEST, the header of the records it captures
        Storage::DataBase::Instance().SetVariant(std::move(key));
        Serialization::g_variant_folder = profile.heap_images != 0 || profile.shader_addr != 0;
    }
    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    // A cache made for another profile (another GPU or driver, a new build) cannot be used. It
    // used to be ignored and left in place, so every later run started without a cache and saved
    // nothing: it is set aside and a new one starts with this profile.
    bool compatible = true;
    if (!profile_data.empty()) {
        Shader::Profile cached_profile{};
        // sotc: SOTC_BDA added Profile::bda_buffers after stage_bindings (64 -> 68 bytes): a cache written
        // before it matches with the flag off instead of being discarded.
        if (profile_data.size() == offsetof(Shader::Profile, bda_buffers)) {
            profile_data.resize(sizeof(Shader::Profile), 0);
        }
        // sotc: SOTC_N5_BINDLESS added Profile::bindless_images (68 -> 72 bytes with the padding).
        if (profile_data.size() == offsetof(Shader::Profile, bindless_images)) {
            profile_data.resize(sizeof(Shader::Profile), 0);
        }
        // sotc: ARCH-GCN-NATIVE added Profile::gcn_buf and gcn_tex after shader_addr (68 -> 72 bytes): a cache written
        // before them matches with both off instead of being discarded.
        if (profile_data.size() == offsetof(Shader::Profile, gcn_buf)) {
            profile_data.resize(sizeof(Shader::Profile), 0);
        }
        if (profile_data.size() == sizeof(Shader::Profile)) {
            // sotc: barycentric_via_gs, the last member, sits where older caches have a padding
            // byte (seen 0xe3, 0x36): anything but 1 there is off, so those caches still match.
            auto& via_gs = profile_data[offsetof(Shader::Profile, barycentric_via_gs)];
            via_gs = via_gs == 1 ? 1 : 0;
            // sotc: stage_bindings sits in the padding byte after it (see Profile).
            auto& stage_bindings = profile_data[offsetof(Shader::Profile, stage_bindings)];
            stage_bindings = stage_bindings == 1 ? 1 : 0;
            auto& bda_buffers = profile_data[offsetof(Shader::Profile, bda_buffers)]; // sotc: SOTC_BDA
            bda_buffers = bda_buffers == 1 ? 1 : 0;
            auto& bindless_images = profile_data[offsetof(Shader::Profile, bindless_images)];
            bindless_images = bindless_images == 1 ? 1 : 0;
            // sotc: SOTC_HEAP and SOTC_SHADER_ADDR (the last two bytes, 66 and 67; measured with clang:
            // sizeof(Shader::Profile) is 68). Anything but 1 is off, as for the flags above.
            auto& heap_images = profile_data[offsetof(Shader::Profile, heap_images)];
            // sotc: 2 is the GPU verify (SOTC_HEAP_GPU_VERIFY): kept, or its profile would load the wrong SPIR-V.
            heap_images = heap_images <= 2 ? heap_images : 0;
            // sotc: shader_addr is the mode (0, 1 or 2); anything else is off, as for the flags above.
            auto& shader_addr = profile_data[offsetof(Shader::Profile, shader_addr)];
            // sotc: 3 and 4 are the table modes of SOTC_ADDR_TABLE (addr_table.h); without them a saved profile
            // would lose its mode and load the wrong SPIR-V.
            shader_addr = (shader_addr >= 1 && shader_addr <= 4) ? shader_addr : 0;
            // sotc: ARCH-GCN-NATIVE. gcn_buf and gcn_tex are the bytes 68 and 69 (sizeof is 72 since they were added):
            // 0 off, 1 on, 2 verify; anything else is off.
            auto& gcn_buf = profile_data[offsetof(Shader::Profile, gcn_buf)];
            gcn_buf = gcn_buf <= 2 ? gcn_buf : 0;
            auto& gcn_tex = profile_data[offsetof(Shader::Profile, gcn_tex)];
            gcn_tex = gcn_tex <= 2 ? gcn_tex : 0;
            std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        }
        compatible = profile_data.size() == sizeof(Shader::Profile) && cached_profile == profile;
        if (!compatible) {
            LOG_WARNING(Render, "Pipeline cache isn't compatible with current system (profile of "
                                "{} bytes, expected {}). Starting a new cache",
                        profile_data.size(), sizeof(Shader::Profile));
            Storage::DataBase::Instance().Reset();
        }
    }
    if (profile_data.empty() || !compatible) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }

    // sotc: SOTC_HEAP: the heap candidates refused by earlier sessions (PipelineCache::NoteHeapRefused).
    {
        std::vector<u8> refused;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "heap_refused", refused);
        for (size_t offset = 0; offset + sizeof(u64) <= refused.size(); offset += sizeof(u64)) {
            u64 hash{};
            std::memcpy(&hash, refused.data() + offset, sizeof(hash));
            heap_refused.insert(hash);
        }
    }

    PrePackLoad(); // sotc: the warm-up reads the stages that have no loose file from the pretranslate pack
    if (SotcNonBlocking::LazyPso()) {
        LoadRecentPipelines(); // sotc: the pipelines the lazy warm-up still builds
    }
    u32 num_pipelines{};
    u32 num_total_pipelines{};
    const auto warmup_start = std::chrono::steady_clock::now();
    // sotc: progress for the loading screen's bar.
    SotcStats::warmup_done.store(0, std::memory_order_relaxed);
    SotcStats::warmup_total.store(
        static_cast<u32>(Storage::DataBase::Instance().CountBlobs(Storage::BlobType::PipelineKey)),
        std::memory_order_relaxed);

    // sotc: one blob is loaded by load_blob (its use count is the N5 order, 0 without N5).
    const auto load_blob = [&](std::vector<u8>&& data, u32 uses) {
        ++num_total_pipelines;
        SotcStats::warmup_done.fetch_add(1, std::memory_order_relaxed);
        if (SotcNonBlocking::LazyPso()) {
            // sotc: only the pipelines of the last sessions are built now; the rest at their first draw or dispatch.
            // The blob: version (u32), compute flag (u32), then the key (ComputePipelineKey value, or the graphics key).
            u64 id = 0;
            if (data.size() >= 8 + sizeof(u64)) {
                u32 compute_flag{};
                std::memcpy(&compute_flag, data.data() + 4, sizeof compute_flag);
                if (compute_flag) {
                    std::memcpy(&id, data.data() + 8, sizeof id);
                } else if (data.size() >= 8 + sizeof(GraphicsPipelineKey)) {
                    GraphicsPipelineKey key;
                    std::memcpy(&key, data.data() + 8, sizeof key);
                    id = std::hash<GraphicsPipelineKey>{}(key);
                }
            }
            if (!RecentPipeline(id)) {
                return;
            }
        }
        warm_uses = uses;

        bool result{};
        try {
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }
        } catch (const Serialization::CorruptedData&) {
            // A truncated entry (e.g. from a killed session) is just skipped.
            infos.fill(nullptr);
            modules.fill(nullptr);
            fetch_shader = nullptr;
            result = false;
        }

        if (result) {
            ++num_pipelines;
        }
    };
    if (N5PsoEnabled()) {
        // sotc: N5 boot order. Every blob is read first and loaded by its use count, most used
        // first, so the workers build the pipelines the last session needed first. Equal counts
        // keep the store's order.
        struct BootBlob {
            u32 uses;
            std::vector<u8> data;
        };
        std::vector<BootBlob> blobs;
        Storage::DataBase::Instance().ForEachBlob(
            Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
                blobs.push_back({PipelineUseCount(data), std::move(data)});
            });
        std::stable_sort(blobs.begin(), blobs.end(),
                         [](const BootBlob& a, const BootBlob& b) { return a.uses > b.uses; });
        for (auto& blob : blobs) {
            load_blob(std::move(blob.data), blob.uses);
        }
    } else {
        Storage::DataBase::Instance().ForEachBlob(
            Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
                load_blob(std::move(data), 0);
            });
    }

    // sotc: the compute pipelines the workers built (SOTC_ASYNC_PRELOAD); the graphics ones
    // are still being built while the game starts.
    for (const auto& key : preload_computes) {
        const auto it = compute_pipelines.find(key);
        if (it == compute_pipelines.end()) {
            continue;
        }
        it.value()->WaitBuilt();
        if (!it.value()->Handle()) {
            preload_refused.push_back(std::move(it.value()));
            compute_pipelines.erase(it);
            --num_pipelines;
        }
    }
    preload_computes.clear();

    // sotc: N5 boot prebuild: waits for the graphics pipelines queued above (WaitPreloadBoot).
    if (N5PsoEnabled()) {
        WaitPreloadBoot(warmup_start);
    }

    LOG_INFO(Render,
             "Preloaded {} pipelines in {} ms (SPIR-V reads {} ms, shader modules {} ms, "
             "pipeline creation {} ms)",
             num_pipelines, Ms(std::chrono::steady_clock::now() - warmup_start),
             Ms(warmup_times.load), Ms(warmup_times.modules), Ms(warmup_times.pipelines));
    SotcStats::warmup_total.store(0, std::memory_order_relaxed); // the bar goes away
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }
    CacheDiagReport("warm-up");
    LoadShaderIndex(); // sotc: the programs on disk that no pipeline loaded (LoadLazyProgram)
    PrePackLoad();     // sotc: and the SOTC_PRETRANSLATE pack
    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::LoadShaderIndex() {
    std::vector<u32> data;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary, kShaderIndexName, data);
    if (data.size() < 2 || data[0] != 1 || data.size() < 2 + size_t{data[1]} * 3) {
        return;
    }
    std::scoped_lock lock{g_index_mutex};
    for (u32 i = 0; i < data[1]; ++i) {
        const u64 hash = u64{data[2 + i * 3]} | (u64{data[3 + i * 3]} << 32);
        const u32 count = data[4 + i * 3];
        auto& known = g_index[hash];
        known = std::max(known, count);
        lazy_programs[hash] = known; // also a program the pipelines loaded: its other permutations
    }
    std::fprintf(stderr, "SOTC SHADER INDEX: %u programs on disk\n", data[1]);
}

void PipelineCache::SaveShaderIndex() {
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::ShaderIndex};
    std::vector<u32> data;
    {
        std::scoped_lock lock{g_index_mutex};
        if (!g_index_dirty) {
            return;
        }
        g_index_dirty = false;
        data.reserve(2 + g_index.size() * 3);
        data.push_back(1);
        data.push_back(static_cast<u32>(g_index.size()));
        for (const auto& [hash, count] : g_index) {
            data.push_back(static_cast<u32>(hash));
            data.push_back(static_cast<u32>(hash >> 32));
            data.push_back(count);
        }
    }
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary, kShaderIndexName, std::move(data));
}

namespace {
// sotc: SOTC_PRETRANSLATE pack. "SOTCPRE1", then records (u64 hash, u32 perm, u32 meta bytes, u32 spv bytes, meta, spv),
// then the index (u32 count, records of u64 hash, u32 perm, u64 offset, u32 meta bytes, u32 spv bytes), then the
// index offset (u64) and "SOTCPRE1". Written and read on the command processor thread only.
constexpr char kPackMagic[8] = {'S', 'O', 'T', 'C', 'P', 'R', 'E', '1'};
struct PackEntry {
    u64 hash;
    u32 perm;
    u64 offset; // of the meta bytes
    u32 meta_size;
    u32 spv_size;
};
std::FILE* g_pack_out = nullptr;
bool g_pack_closed = false; // finished (or failed) this session: no second writer
std::vector<PackEntry> g_pack_written;
std::FILE* g_pack_in = nullptr;
std::unordered_map<u64, std::vector<PackEntry>> g_pack_index;
std::unordered_map<u64, PackEntry> g_pack_by_stage; ///< HashCombine(hash, perm) -> entry (PackStageMeta / PackStageSpv)
// sotc: SOTC_NB_IO: pack loads also run on workers (RunLazyLoad); reads and the reopen of g_pack_in take this lock.
std::mutex g_pack_mutex;
// sotc: SOTC_NB_IO counters (CP), printed every 5 s by InstallLazyLoad.
struct LazyStats {
    u64 queued{}, installed{}, already_loaded{}, dropped{}, read_failed{};
    std::chrono::steady_clock::time_point last{std::chrono::steady_clock::now()};
} g_lazy_stats;

std::filesystem::path PackPath() {
    return Storage::DataBase::Instance().CachePath() / "sotc_pretranslate.pack";
}

bool PackRead(u64 offset, void* out, size_t n) {
    std::scoped_lock lock{g_pack_mutex};
    return g_pack_in && _fseeki64(g_pack_in, static_cast<s64>(offset), SEEK_SET) == 0 &&
           std::fread(out, 1, n, g_pack_in) == n;
}

bool PackStageMeta(u64 stage_hash, std::vector<u8>& meta) {
    const auto it = g_pack_by_stage.find(stage_hash);
    if (it == g_pack_by_stage.end()) {
        return false;
    }
    meta.resize(it->second.meta_size);
    if (!PackRead(it->second.offset, meta.data(), it->second.meta_size)) {
        meta.clear();
        return false;
    }
    return true;
}

bool PackStageSpv(u64 stage_hash, std::vector<u32>& spv) {
    const auto it = g_pack_by_stage.find(stage_hash);
    if (it == g_pack_by_stage.end()) {
        return false;
    }
    spv.resize(it->second.spv_size / sizeof(u32));
    if (!PackRead(it->second.offset + it->second.meta_size, spv.data(), it->second.spv_size)) {
        spv.clear();
        return false;
    }
    return true;
}
} // namespace

bool PipelineCache::PrePackAdd(u64 hash, u32 perm_idx, std::vector<u8>&& meta, const std::vector<u32>& spv) {
    // sotc: SOTC_PRETRANSLATE_PACK=0 writes the hold's variants as loose files instead (diagnostics).
    static const bool pack_on = [] {
        const char* v = std::getenv("SOTC_PRETRANSLATE_PACK");
        return !v || v[0] != '0';
    }();
    if (!pack_on || g_pack_closed || !Storage::DataBase::Instance().IsOpened()) {
        return false;
    }
    if (!g_pack_out) {
        std::error_code ec;
        std::filesystem::create_directories(PackPath().parent_path(), ec);
        g_pack_out = std::fopen(PackPath().string().c_str(), "wb");
        if (!g_pack_out || std::fwrite(kPackMagic, 1, sizeof kPackMagic, g_pack_out) != sizeof kPackMagic) {
            g_pack_closed = true;
            return false;
        }
    }
    const u32 meta_size = static_cast<u32>(meta.size());
    const u32 spv_size = static_cast<u32>(spv.size() * sizeof(u32));
    std::fwrite(&hash, sizeof hash, 1, g_pack_out);
    std::fwrite(&perm_idx, sizeof perm_idx, 1, g_pack_out);
    std::fwrite(&meta_size, sizeof meta_size, 1, g_pack_out);
    std::fwrite(&spv_size, sizeof spv_size, 1, g_pack_out);
    const u64 offset = static_cast<u64>(_ftelli64(g_pack_out));
    std::fwrite(meta.data(), 1, meta_size, g_pack_out);
    std::fwrite(spv.data(), 1, spv_size, g_pack_out);
    g_pack_written.push_back({hash, perm_idx, offset, meta_size, spv_size});
    return true;
}

void PipelineCache::PrePackFinish() {
    if (!g_pack_out) {
        return;
    }
    const u64 index_offset = static_cast<u64>(_ftelli64(g_pack_out));
    const u32 count = static_cast<u32>(g_pack_written.size());
    std::fwrite(&count, sizeof count, 1, g_pack_out);
    for (const auto& e : g_pack_written) {
        std::fwrite(&e.hash, sizeof e.hash, 1, g_pack_out);
        std::fwrite(&e.perm, sizeof e.perm, 1, g_pack_out);
        std::fwrite(&e.offset, sizeof e.offset, 1, g_pack_out);
        std::fwrite(&e.meta_size, sizeof e.meta_size, 1, g_pack_out);
        std::fwrite(&e.spv_size, sizeof e.spv_size, 1, g_pack_out);
    }
    std::fwrite(&index_offset, sizeof index_offset, 1, g_pack_out);
    std::fwrite(kPackMagic, 1, sizeof kPackMagic, g_pack_out);
    const bool ok = std::fclose(g_pack_out) == 0;
    g_pack_out = nullptr;
    g_pack_closed = true;
    std::fprintf(stderr, "SOTC PRETRANSLATE pack: %u variants written%s\n", count, ok ? "" : " (write failed)");
    g_pack_written.clear();
    if (ok) {
        PrePackLoad();
    }
}

void PipelineCache::PrePackLoad() {
    {
        std::scoped_lock lock{g_pack_mutex};
        if (g_pack_in) {
            std::fclose(g_pack_in);
            g_pack_in = nullptr;
        }
    }
    g_pack_index.clear();
    g_pack_by_stage.clear();
    std::FILE* f = std::fopen(PackPath().string().c_str(), "rb");
    if (!f) {
        return;
    }
    char magic[8]{};
    u64 index_offset = 0;
    u32 count = 0;
    const bool ok = _fseeki64(f, -16, SEEK_END) == 0 && std::fread(&index_offset, sizeof index_offset, 1, f) == 1 &&
                    std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kPackMagic, 8) == 0 &&
                    _fseeki64(f, static_cast<s64>(index_offset), SEEK_SET) == 0 &&
                    std::fread(&count, sizeof count, 1, f) == 1;
    if (!ok) {
        std::fclose(f);
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        PackEntry e{};
        if (std::fread(&e.hash, sizeof e.hash, 1, f) != 1 || std::fread(&e.perm, sizeof e.perm, 1, f) != 1 ||
            std::fread(&e.offset, sizeof e.offset, 1, f) != 1 ||
            std::fread(&e.meta_size, sizeof e.meta_size, 1, f) != 1 ||
            std::fread(&e.spv_size, sizeof e.spv_size, 1, f) != 1) {
            break;
        }
        auto& list = g_pack_index[e.hash];
        list.push_back(e);
        g_pack_by_stage[HashCombine(e.hash, u64{e.perm})] = e;
        auto& known = lazy_programs[e.hash];
        known = std::max(known, e.perm + 1);
    }
    {
        std::scoped_lock lock{g_pack_mutex};
        g_pack_in = f;
    }
    std::fprintf(stderr, "SOTC PRETRANSLATE pack: %zu programs ready on disk\n", g_pack_index.size());
}

bool PipelineCache::LoadLazyProgram(u64 hash) {
    const auto entry = lazy_programs.find(hash);
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::LazyLoad};
    if (entry == lazy_programs.end()) {
        return false;
    }
    const u32 count = entry->second;
    lazy_programs.erase(entry);
    const auto packed = g_pack_index.find(hash);
    bool loaded = false;
    for (u32 idx = 0; idx < count; ++idx) {
        const auto existing = program_cache.find(hash);
        if (existing != program_cache.end() && idx < existing.value()->modules.size() &&
            existing.value()->modules[idx].module) {
            continue; // a warm-up pipeline loaded it
        }
        std::vector<u8> meta_blob;
        std::vector<u32> packed_spv;
        if (packed != g_pack_index.end()) {
            for (const auto& e : packed->second) {
                if (e.perm != idx) {
                    continue;
                }
                meta_blob.resize(e.meta_size);
                packed_spv.resize(e.spv_size / sizeof(u32));
                if (!PackRead(e.offset, meta_blob.data(), e.meta_size) ||
                    !PackRead(e.offset + e.meta_size, packed_spv.data(), e.spv_size)) {
                    meta_blob.clear();
                    packed_spv.clear();
                }
                break;
            }
            // sotc: no loose copies: the pipeline warm-up of the next sessions reads the stages from the pack when
            // a loose file is missing (PackStageMeta / PackStageSpv).
        }
        if (meta_blob.empty()) {
            Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                               fmt::format("{:#018x}", HashCombine(hash, u64{idx})), meta_blob);
        }
        if (meta_blob.empty()) {
            continue;
        }
        auto program = std::make_unique<Program>();
        Shader::StageSpecialization spec{};
        spec.info = &program->info;
        size_t perm_idx{};
        try {
            Serialization::Archive ar{std::move(meta_blob)};
            if (!LoadShaderMeta(ar, program->info, spec, perm_idx) || perm_idx != idx) {
                continue;
            }
        } catch (...) {
            continue;
        }
        std::vector<u32> spv = std::move(packed_spv);
        if (spv.empty()) {
            Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                               fmt::format("{:#018x}_{}", hash, idx), spv);
        }
        if (spv.empty()) {
            continue;
        }
        AsyncPipelineJob::LazyLoad::Perm perm{idx, std::move(program), std::move(spec), {}};
        const auto existing_pgm = program_cache.find(hash);
        if (existing_pgm != program_cache.end() && !SameBindingLayout(existing_pgm.value()->info, perm.program->info)) {
            InstallLazyPerm(hash, perm); // logs and refuses it, without creating the module
            continue;
        }
        perm.module = CompileSPV(spv, instance.GetDevice());
        if (InstallLazyPerm(hash, perm)) {
            loaded = true;
        } else if (perm.module) {
            instance.GetDevice().destroyShaderModule(perm.module);
        }
    }
    return loaded;
}

bool PipelineCache::InstallLazyPerm(u64 hash, AsyncPipelineJob::LazyLoad::Perm& perm) {
    auto [it_pgm, is_new] = program_cache.try_emplace(hash);
    // sotc: a permutation whose resources differ from the program's would be drawn with descriptors written for
    // the program's Info (SameBindingLayout): it is not loaded (a draw that needs it translates it again).
    if (!is_new && !SameBindingLayout(it_pgm.value()->info, perm.program->info)) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
            std::fprintf(stderr, "SOTC SHADER CACHE: permutation %u of %#llx not loaded, its resources differ from "
                         "the program's\n", perm.idx, static_cast<unsigned long long>(hash));
        }
        return false;
    }
    if (!perm.module) {
        if (is_new) {
            program_cache.erase(hash);
        }
        return false;
    }
    if (is_new) {
        it_pgm.value() = std::move(perm.program);
        // sotc: a program of the SOTC_PRETRANSLATE pack counts as pre-translated: its first draw compares the draw's
        // key with the packed ones (the "first draws / key hit" of the pretranslate report, 0 before: the boot hold
        // evicts its programs, and they came back from the pack without the mark). Only after that hold: its eviction
        // takes the marked programs no draw used, and a program loaded for the manifest may still be in a job then.
        if (pre_hold_done && !pre_holding && g_pack_index.contains(hash)) {
            it_pgm.value()->sotc_pre = true;
        }
    } else {
        Shader::CombineBindlessCache(it_pgm.value()->info.bindless, perm.program->info.bindless);
    }
    perm.spec.info = &it_pgm.value()->info;
    it_pgm.value()->InsertPermut(perm.module, std::move(perm.spec), perm.idx);
    perm.module = vk::ShaderModule{}; // owned by the program now
    return true;
}

bool PipelineCache::QueueLazyPrefetch(u64 hash) {
    const auto lazy = lazy_programs.find(hash);
    if (lazy == lazy_programs.end() || lazy_prefetch.contains(hash) || async_workers.empty()) {
        return false;
    }
    const auto packed = g_pack_index.find(hash);
    if (packed == g_pack_index.end()) {
        return false; // loose files only: its first draw loads it (Storage reads stay on the command processor)
    }
    auto load = std::make_unique<AsyncPipelineJob::LazyLoad>();
    load->hash = hash;
    for (u32 idx = 0; idx < lazy->second; ++idx) {
        const auto e = std::ranges::find(packed->second, idx, &PackEntry::perm);
        if (e == packed->second.end()) {
            return false; // a permutation outside the pack: the first draw loads them all
        }
        load->entries.push_back({e->perm, e->offset, e->meta_size, e->spv_size});
    }
    auto job = std::make_unique<AsyncPipelineJob>();
    job->lazy = std::move(load);
    lazy_prefetch.insert(hash);
    ++g_lazy_stats.queued;
    lazy_backlog.push_back(std::move(job));
    FeedLazyPrefetch();
    return true;
}

void PipelineCache::FeedLazyPrefetch() {
    // At most 2 pack loads on the workers: the rest of the workers stay free for the jobs the frame waits for
    // (compute pipelines, translations). The first P1 run gave every worker to 11,465 loads at once.
    constexpr u32 kInFlight = 2;
    bool fed = false;
    while (lazy_in_flight < kInFlight && !lazy_backlog.empty()) {
        {
            std::scoped_lock lock{async_mutex};
            PushJobLocked(JobLane::Background, std::move(lazy_backlog.front())); // behind the draws' own jobs
        }
        lazy_backlog.pop_front();
        ++lazy_in_flight;
        fed = true;
    }
    if (fed) {
        async_cv.notify_all();
    }
}

void PipelineCache::RunLazyLoad(AsyncPipelineJob::LazyLoad& load) {
    for (const auto& e : load.entries) {
        std::vector<u8> meta_blob(e.meta_size);
        std::vector<u32> spv(e.spv_size / sizeof(u32));
        if (!PackRead(e.offset, meta_blob.data(), e.meta_size) ||
            !PackRead(e.offset + e.meta_size, spv.data(), e.spv_size)) {
            continue;
        }
        // No loose copies (the warm-up reads the pack, PackStageMeta / PackStageSpv).
        AsyncPipelineJob::LazyLoad::Perm perm;
        perm.idx = e.perm;
        perm.program = std::make_unique<Program>();
        perm.spec.info = &perm.program->info;
        size_t perm_idx{};
        try {
            Serialization::Archive ar{std::move(meta_blob)};
            if (!LoadShaderMeta(ar, perm.program->info, perm.spec, perm_idx) || perm_idx != e.perm) {
                continue;
            }
        } catch (...) {
            continue;
        }
        perm.module = CompileSPV(spv, instance.GetDevice());
        load.perms.push_back(std::move(perm));
    }
}

void PipelineCache::InstallLazyLoad(AsyncPipelineJob::LazyLoad& load) {
    lazy_prefetch.erase(load.hash);
    if (lazy_in_flight > 0) {
        --lazy_in_flight;
    }
    FeedLazyPrefetch();
    // A draw that came first loaded the program inline (LoadLazyProgram erased its entry): its permutations are there.
    lazy_programs.erase(load.hash);
    if (load.perms.size() < load.entries.size()) {
        ++g_lazy_stats.read_failed;
    }
    for (auto& perm : load.perms) {
        const auto existing = program_cache.find(load.hash);
        if (existing != program_cache.end() && perm.idx < existing.value()->modules.size() &&
            existing.value()->modules[perm.idx].module) {
            ++g_lazy_stats.already_loaded;
        } else if (InstallLazyPerm(load.hash, perm)) {
            ++g_lazy_stats.installed;
            continue;
        } else {
            ++g_lazy_stats.dropped;
        }
        if (perm.module) {
            instance.GetDevice().destroyShaderModule(perm.module);
            perm.module = vk::ShaderModule{};
        }
    }
    // A permutation the worker could not read is left to the program's draws (translated again if needed).
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lazy_stats.last >= std::chrono::seconds(5)) {
        g_lazy_stats.last = now;
        std::fprintf(stderr,
                     "SOTC NB IO: pack prefetch queued %llu, permutations installed %llu, already loaded by a draw "
                     "%llu, refused %llu, programs with a read failure %llu, queued or running %zu\n",
                     static_cast<unsigned long long>(g_lazy_stats.queued),
                     static_cast<unsigned long long>(g_lazy_stats.installed),
                     static_cast<unsigned long long>(g_lazy_stats.already_loaded),
                     static_cast<unsigned long long>(g_lazy_stats.dropped),
                     static_cast<unsigned long long>(g_lazy_stats.read_failed), lazy_prefetch.size());
    }
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(FlatBuf());
    srt_info.Serialize(ar);
    info.Write(variant); // sotc: SOTC_HEAP, not part of InfoPersistent
    info.Write(has_discard);        // sotc: version 25, not part of InfoPersistent either
    info.Write(has_storage_images); // sotc: version 25
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};
    bindless.ready = false; // sotc: the record replaces the Info; its answers are filled below

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(FlatBuf());
    // sotc: has_flatbuf is an Info member, not part of InfoPersistent, so the record above leaves it false. A cached
    // shader then reads as "no flat buffer" in ShaderAddrBuffer (host and recompiler), and the host binds its
    // descriptors and never makes the mirror resident: shader-addressed reads return zero. The flag is set exactly
    // when AddFlatbuf adds the Flatbuf resource (shader_info_collection_pass.cpp), and that resource is in the record.
    has_flatbuf = false;
    for (const auto& desc : buffers) {
        if (desc.buffer_type == BufferType::Flatbuf) {
            has_flatbuf = true;
            break;
        }
    }

    if (!srt_info.Deserialize(ar)) {
        return false;
    }
    info.Read(variant); // sotc: SOTC_HEAP, see Serialize
    info.Read(has_discard);        // sotc: version 25, see Serialize
    info.Read(has_storage_images); // sotc: version 25
    // sotc: the record is complete: the bindless answers, as the translation fixed them for this flat buffer.
    FillBindlessCache();
    return true;
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
    fetch.Write(one_vgprs);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);
    fetch.Read(one_vgprs);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
        if (!walker_func) {
            // No room for the walker: drop the cached program, it is compiled again.
            walker_func_size = 0;
            return false;
        }
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
