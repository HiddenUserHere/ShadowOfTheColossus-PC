// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_set>
#include "common/hash.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/sotc_stats.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// Above both lines before the upstream merge: generation changed, so old caches are dropped.
// sotc: 33 = the draw thread merge (SRT walker faults read zero instead of patching). 34 = the
// LOD feedback (sotc_lod_stats.h); binaries built with SOTC_LOD_FEEDBACK=0 are another version.
// 37/135 = invariant position on barycentric-emulation pipelines (ground-level texture/depth
// mismatch between the pipeline with the geometry shader and the one without; PR #258).
// 38/136 = SRT walker resolves Phis whose operands compute the same offset (flattened layout
// changes for those shaders).
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
// sotc: 39/137 (from 38/136): the SRT walker no longer uses dynamic-window ReadConsts as offsets.
// 40/138: the cache now takes loose files newer than the pack (cache_storage.cpp), which brought
// back binaries written by older recompiler builds under the same number (wrong vertices, dark
// trees): everything is translated again once.
// 41 (from 40): early fragment tests on the LOD feedback shaders (emit_spirv.cpp, upstream
// 0.70's 38); the non-feedback binaries are unchanged.
static const u32 ShaderBinaryVersion = (Shader::LodFeedbackEnabled() ? 41u : 138u) +
                                       (Shader::StageBindingsEnabled() ? 1000u : 0u) +
                                       (BindlessSamplersRequested() ? 2000u : 0u) +
                                       (SrtPhiDisabled() ? 4000u : 0u) +
                                       (AddrVertexRequested() ? 8000u : 0u);
// sotc: 22 (from 21): the record ends with the stage variant (SOTC_HEAP). Without it a cached Plain or Heap
// program came back as Default, so its layout disagreed with its binary (slot-word assert, device lost).
// 23: a variant's pgm_hash is the game's hash again (Info::CacheHash keys the caches), so 22's variant records differ.
// 24: BufferResource::sotc_read_extent (SOTC_SHADER_EXTENT) makes the buffer records 4 bytes longer; cached programs
// are translated once again (the SPIR-V itself is unchanged).
// 25: the record ends with has_discard and has_storage_images. They are Info members outside InfoPersistent, so every
// cached shader came back with both false: SOTC_NO_PREPASS=3 took alpha-tested pre-pass draws for opaque ones and
// dropped them (grass without alpha, 3 Nov), and the host's storage-image checks (op writes, fast bind, bind cache)
// missed cached shaders.
static constexpr u32 ShaderMetaVersion = 25u;
static const u32 PipelineKeyVersion =
    3u + (Shader::StageBindingsEnabled() ? 1000u : 0u) + (BindlessSamplersRequested() ? 2000u : 0u);
} // namespace Serialization

namespace Vulkan {

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
bool SlotHasCurrentMeta(u64 pgm_hash, size_t perm_idx) {
    std::vector<u8> blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", HashCombine(pgm_hash, u64(perm_idx))),
                                       blob);
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
        return meta_version == Serialization::ShaderMetaVersion &&
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

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
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

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

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
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    return info.Deserialize(ar);
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
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, layout_cache, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, true, deferred);
        it.value()->use_count = warm_uses;
        it.value()->saved_use_count = warm_uses;
        if (deferred) {
            auto job = std::make_unique<AsyncPipelineJob>();
            job->compute = it.value().get();
            {
                std::scoped_lock lock{async_mutex};
                async_queue.insert(async_queue.begin() + async_compute_queued++, std::move(job));
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
            instance, scheduler, desc_heap, layout_cache, profile, graphics_key, *pipeline_cache, infos,
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
        return false;
    }

    std::vector<u32> spv{};
    {
        WarmUpTimer timer{warmup_times.load};
        Storage::DataBase::Instance().Load(
            Storage::BlobType::ShaderBinary,
            fmt::format("{:#018x}_{}", program->info.CacheHash(), perm_idx), spv);
    }
    if (spv.empty()) {
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
        for (const char* name : {"SOTC_WQM_COPY", "SOTC_WAVE64_UNIFORM", "SOTC_WAVE64_IDENTITY",
                                 "SOTC_TSHARP_ARRAY", "SHADPS4_IEEE_MINMAX"}) {
            if (const char* v = std::getenv(name); v && *v) {
                key += fmt::format(" {}={}", name, v);
            }
        }
        Storage::DataBase::Instance().SetVariant(std::move(key));
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
    Storage::DataBase::Instance().FinishPreload();
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
