// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem> // sotc: SOTC_PLAY_QUIET (SaveCacheFile)
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include "../../../sotc/pipe_manifest.h" // sotc: SOTC_PIPE_MANIFEST
#include "../../../sotc/pipe_predict.h" // sotc: SOTC_GPL_PREDICT
#include "../../../sotc/pretranslate.h" // sotc: SOTC_PRETRANSLATE
#include <unordered_set>
#include <variant>
#include <vector>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "vulkan/vulkan.hpp"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
struct ComputeProgram;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

/// sotc: two translations of one program bind the same descriptors: the same resource kinds and counts. The
/// rasterizer writes a draw's descriptors from the program's Info, so a permutation whose own Info differs (a
/// SOTC_PRETRANSLATE variant whose translation found a flat buffer the program has not) must not join the program.
inline bool SameBindingLayout(const Shader::Info& a, const Shader::Info& b) {
    if (a.buffers.size() != b.buffers.size() || a.images.size() != b.images.size() ||
        a.samplers.size() != b.samplers.size() || a.fmasks.size() != b.fmasks.size()) {
        return false;
    }
    for (size_t i = 0; i < a.buffers.size(); ++i) {
        if (a.buffers[i].buffer_type != b.buffers[i].buffer_type) {
            return false;
        }
    }
    for (size_t i = 0; i < a.images.size(); ++i) {
        if (a.images[i].NumBindings(a) != b.images[i].NumBindings(b) || a.images[i].is_written != b.images[i].is_written) {
            return false;
        }
    }
    return true;
}

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
        /// sotc: SOTC_COMPUTE_RELEASE: a compute permutation whose module was destroyed once its pipeline was built
        /// (module is null; the pipeline in compute_pipelines serves it). It counts as present for the lookups.
        bool sotc_released{};
        /// sotc: the permutation exists: a module, or a released one (SOTC_COMPUTE_RELEASE).
        bool Present() const {
            return module || sotc_released;
        }
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    /// sotc: permutations being translated by a pipeline worker (SOTC_ASYNC_TRANSLATE).
    std::vector<Shader::StageSpecialization> pending_specs;
    /// sotc: a worker read other tables than the draw that asked (the game reused them before
    /// the worker ran): this program's later permutations are translated in place.
    bool translate_in_place{};
    /// sotc: SOTC_PRETRANSLATE: installed by a translation at resource load (gpu/sotc/pretranslate.h); checked set
    /// once its first draw compared its key with the pre-translated one.
    bool sotc_pre{};
    bool sotc_pre_checked{};
    /// sotc: SOTC_COMPUTE_SPECFREE: the compute prebuild's ambiguous images (Info::sotc_synthetic_ambiguous); above
    /// kSpecFreeMaxAmbiguous only the two all-int/all-float variants are prebuilt (fallback reasons).
    u8 sotc_ambiguous{};

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
        if (new_perm_log) {
            new_perm_log->emplace_back(info.CacheHash(), modules.size() - 1);
        }
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
        if (new_perm_log) {
            new_perm_log->emplace_back(info.CacheHash(), perm_idx);
        }
    }

    /// sotc: SOTC_NB_GPL (P4): every new permutation (program cache hash, index), for the library warm-up. Programs get
    /// modules on the command processor only; null when the warm-up is off.
    static inline std::vector<std::pair<u64, size_t>>* new_perm_log = nullptr;
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

/// sotc: SOTC_PIPE_DIRTY. The registers RefreshGraphicsKeyRegs reads, copied when the register part
/// of the graphics key was built. Equal registers give an equal register part (no memory is read).
/// Extend this struct (and PipeKeyRegsMatch / PipeKeyRegsCapture) when that function reads another
/// register: a missing member would let a stale key through.
struct PipeKeyRegs {
    AmdGpu::DepthBuffer depth_buffer;
    AmdGpu::DepthRenderOverride depth_render_override;
    AmdGpu::ClipperControl clipper_control;
    AmdGpu::PolygonControl polygon_control;
    AmdGpu::PrimitiveType primitive_type;
    AmdGpu::ShaderStageEnable stage_enable;
    AmdGpu::LsHsConfig ls_hs_config;
    AmdGpu::ColorControl color_control;
    AmdGpu::ColorBufferMask color_shader_mask;
    AmdGpu::ColorBufferMask color_target_mask;
    AmdGpu::ColorExportFormat color_export_format;
    std::array<AmdGpu::ColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_control;
};

/// sotc: SOTC_PIPE_FAST. Every register a graphics lookup reads besides the key part: the key part
/// (PipeKeyRegs) and the stage selection (GS mode and stream-out config, see RefreshGraphicsStages).
struct PipeFastRegs {
    PipeKeyRegs key;
    AmdGpu::GsMode vgt_gs_mode;
    AmdGpu::StreamOutConfig vgt_strmout_config;
};

/// sotc: SOTC_PIPE_FAST: why a draw could not take the recorded lookup (counted per reason).
enum class PipeFastMiss : u8 { None, Record, Async, Regs, Params, Sharp, Runtime };

/// sotc: SOTC_SPEC_MEMO. One stored answer of GetProgram: the permutation a stage resolved to, keyed
/// by the program, the stage pair, the shader hash, the bindings the stage started from, the runtime
/// info, the tessellation constant buffer (tessellation stages), the parsed fetch shader and the key
/// words of the specialization (PackSpecObject layout). Linked in an LRU list by index.
struct SpecMemoEntry {
    static constexpr u32 None = ~u32{0};
    const Program* program{};
    Shader::HwStage hw_stage{};
    Shader::SwStage sw_stage{};
    u64 hash{};
    u32 perm{};
    u64 key_hash{};              ///< the hash the index maps to this entry
    u32 prev{None};              ///< LRU neighbours (towards the most recently used)
    u32 next{None};
    Shader::Backend::Bindings start{};
    Shader::RuntimeInfo runtime_info{};
    bool has_tess{};
    Shader::TessellationDataConstantBuffer tess{};
    Shader::Gcn::FetchShaderData fetch{};
    std::vector<u32> words;
};

/// sotc: SOTC_SPEC_MEMO state: a fixed number of entries (kCap), an index from key hash to entry
/// (several entries may share a hash; each lookup compares the full key), and the LRU ends.
struct SpecMemoState {
    static constexpr size_t kCap = 4096;
    std::vector<SpecMemoEntry> entries; ///< allocated on the first lookup
    std::unordered_multimap<u64, u32> index;
    u32 used{};                         ///< entries filled so far (below kCap, no eviction yet)
    u32 head{SpecMemoEntry::None};      ///< most recently used
    u32 tail{SpecMemoEntry::None};      ///< least recently used: the eviction victim
};

/// sotc: steady clock in ns, for AsyncPipelineJob::created_ns.
u64 SotcJobClockNs() noexcept;

class PipelineCache {
public:
    /// sotc: SOTC_RAM_REPORT: counts of the programs and pipelines kept in host memory, and the size
    /// of the last driver pipeline cache blob. The SPIR-V of a program is not kept (only the module
    /// handles, which the driver holds).
    void SotcCounts(u64& programs, u64& graphics, u64& compute, u64& driver_blob) const noexcept {
        programs = program_cache.size();
        graphics = graphics_pipelines.size();
        compute = compute_pipelines.size();
        driver_blob = saved_driver_cache_size;
    }

    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    /// sotc: the descriptor heap the pipelines bind their sets from (the rasterizer sets the
    /// bindless sampler set on it).
    DescriptorHeap& GetDescriptorHeap() noexcept {
        return desc_heap;
    }

    void WarmUp();

    /// sotc: writes the driver's compiled pipelines to disk (sotc_vk_pipelines.bin), so the next
    /// start, also after an update of the port, does not compile them again.
    // sotc: true only after a successful write; non-null written_size forces an unchanged-size save too.
    // written_size is set under the save mutex on success (pack list publication).
    bool SaveDriverCache(bool final_save = false, size_t* written_size = nullptr, u64* written_hash = nullptr);
    /// sotc: SOTC_PLAY_QUIET: writes the pack cache (stage libraries) to sotc_vk_pack.bin; false without one.
    bool SavePackCache(size_t* written_size = nullptr, u64* written_hash = nullptr);
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {});

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule, u64>;
    /// sotc: async_ok: a stage that needs translating is queued for a pipeline worker instead
    /// (SOTC_ASYNC_TRANSLATE); it returns a null Info and sets translation_pending meanwhile.
    Result GetProgram(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding,
                      bool async_ok = false);
    /// sotc: SOTC_COMPUTE_SPECFREE: counts (and logs the first 32 of) a compute permutation a dispatch needs that its
    /// program's prebuilt one does not serve, by reason (vk_compute_pipeline.h ComputeSpecFreeStats).
    void NoteSpecFreeFallback(const Program& program, const Shader::StageSpecialization& spec, u64 hash);

    /// sotc: SOTC_ASYNC_COMPUTE_XLATE=1 (off by default; needs the pipeline workers). When a
    /// dispatch needs a compute shader not translated yet, the command processor scans the
    /// rest of its command buffer and queues the new compute shaders it finds there for the
    /// workers (PrefetchComputeProgram), while it translates the current one itself. Dispatches
    /// are still published in order: a later dispatch whose shader is still on a worker waits
    /// for it (or takes it back when no worker started it yet). Without the switch, nothing
    /// changes.
    bool ComputePrefetchEnabled() const {
        return compute_prefetch;
    }
    /// sotc: the shader is translated, queued for a worker, or left to the command processor.
    bool IsProgramKnown(u64 hash) const {
        return program_cache.contains(hash) || lazy_programs.contains(hash) || pending_programs.contains(hash) ||
               in_place_programs.contains(hash);
    }
    /// sotc: queues the translation of cs's shader for a worker when it is new. Returns true
    /// when it queued one. Command processor thread only.
    bool PrefetchComputeProgram(const AmdGpu::ComputeProgram& cs);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    /// sotc: N5. The lookups; GetGraphicsPipeline and GetComputePipeline count the draws that bind.
    const GraphicsPipeline* GetGraphicsPipelineLookup(const DrawIndirectParams params);
    const ComputePipeline* GetComputePipelineLookup();
    /// sotc: SOTC_PIPE_FAST. The full lookups the wrappers above run when no recorded lookup applies.
    const GraphicsPipeline* GetGraphicsPipelineLookupSlow(const DrawIndirectParams params);
    const ComputePipeline* GetComputePipelineLookupSlow();
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    /// sotc: the register part of the graphics key: everything RefreshGraphicsKey derives from the
    /// registers before the shader stages are bound. Reads no guest memory.
    void RefreshGraphicsKeyRegs(GraphicsPipelineKey& key) const;
    /// sotc: SOTC_PIPE_DIRTY=1/2. Fills graphics_key's register part: from the cached copy when
    /// PipeKeyRegs still matches the registers, else rebuilt and cached. =2 also rebuilds on a hit
    /// and counts a different result (the rebuilt one is used).
    void RefreshGraphicsKeyRegsDirty(int mode);
    bool pipe_dirty_valid{};        ///< pipe_dirty_regs and pipe_dirty_key hold a built register part
    PipeKeyRegs pipe_dirty_regs{};  ///< the registers the cached register part was built from
    GraphicsPipelineKey pipe_dirty_key{}; ///< the register part (stage fields zero)

    /// sotc: SOTC_PIPE_MEMO=1/2 (see GetProgram). The permutation a stage last resolved to, with
    /// every input its specialization reads: the program and stage pair, the shader hash, the user
    /// data, the flattened buffer (refreshed on every call, compared after that), the runtime info,
    /// the bindings the stage started from, for tessellation the constant buffer in guest memory,
    /// and for a vertex fetch shader its code words. The runtime memory generation of the store
    /// must still be the current one. Command processor thread only.
    struct StageMemo {
        bool valid{};
        const Program* program{};
        Shader::HwStage hw_stage{};
        Shader::SwStage sw_stage{};
        u64 hash{};
        u32 perm{};
        size_t modules_size{}; ///< program->modules.size() when stored (a new permutation moves it)
        std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
        std::vector<u32> flat_buf;
        Shader::RuntimeInfo runtime_info{};
        Shader::Backend::Bindings start{};
        Shader::Backend::Bindings after{}; ///< the bindings the stage left with (slow path result)
        bool has_tess{};
        Shader::TessellationDataConstantBuffer tess{};
        bool has_fetch{};            ///< a vertex fetch shader was parsed (fetch_code is used)
        std::vector<u32> fetch_code; ///< its code words, read from the backing view at store time
        u64 generation{};            ///< runtime_memory_generation() before the slow path ran
        u64 stamp{};                 ///< last store or hit (LRU choice within a set)
    };
    // sotc: 2-way set-associative: kStageMemoSets sets of kStageMemoWays entries. Allocated on the
    // first memo lookup, so the switch being unset costs nothing.
    static constexpr size_t kStageMemoSets = 256;
    static constexpr size_t kStageMemoWays = 2;
    std::vector<StageMemo> stage_memo;
    u64 stage_memo_tick{};
    /// sotc: SOTC_SPEC_MEMO (GetProgram): the specialization memo. Command processor thread only.
    SpecMemoState spec_memo_state;
    /// sotc: the first thread that called GetProgram in a memo mode. Other threads take the slow
    /// path (counted as foreign).
    std::atomic<std::thread::id> pipe_memo_owner{};
    bool PipeMemoOwnerThread();
    static size_t StageMemoSet(u64 hash, Shader::HwStage hw_stage, Shader::SwStage sw_stage);
    bool StageMemoHit(const StageMemo& memo, const Program& program, Shader::HwStage hw_stage,
                      Shader::SwStage sw_stage, const Shader::ShaderParams& params,
                      const Shader::RuntimeInfo& rt, const Shader::Backend::Bindings& start,
                      const Shader::TessellationDataConstantBuffer& tess, bool has_tess,
                      u64 generation) const;
    /// sotc: the way a store goes into when the lookup missed: an entry of the same key (its
    /// inputs changed), else an empty way, else the least recently used one (an eviction).
    StageMemo* StageMemoVictim(StageMemo* ways, const Program& program, Shader::HwStage hw_stage,
                               Shader::SwStage sw_stage, u64 hash);
    /// Returns false (nothing stored) when the fetch code cannot be read from the backing view.
    bool StageMemoStore(StageMemo& memo, const Program& program, Shader::HwStage hw_stage,
                        Shader::SwStage sw_stage, const Shader::ShaderParams& params,
                        const Shader::RuntimeInfo& rt, const Shader::Backend::Bindings& start,
                        const Shader::TessellationDataConstantBuffer& tess, bool has_tess,
                        u64 generation, size_t perm, const Shader::Backend::Bindings& after);
    /// sotc: SOTC_PIPE_MEMO=1/2: the last compute pipeline and the key it was looked up with.
    const ComputePipeline* last_compute_pipeline{};
    ComputePipelineKey last_compute_key{};
    size_t last_compute_size{}; ///< compute_pipelines.size() when stored
    size_t last_graphics_size{}; ///< graphics_pipelines.size() when last_graphics_pipeline was stored

    /// sotc: SOTC_PIPE_FAST. One stage of a resolved lookup: the inputs GetProgram and the
    /// specialization read for it, so the next draw can check them without the full lookup.
    struct PipeFastStage {
        Shader::HwStage hw_stage{};
        Shader::SwStage sw_stage{};
        bool absent{};                   ///< the stage is not bound (no program): nothing to check
        Shader::Info* info{};            ///< the program's info (programs are never erased)
        vk::ShaderModule module{};
        u64 hash{};                      ///< params.hash
        u64 pgm_base{};                  ///< params.Base(): the code address
        std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
        std::vector<u32> flat;           ///< info->FlatBuf() after the lookup's refresh
        Shader::RuntimeInfo runtime_info{}; ///< BuildRuntimeInfo's result for the stage
        bool has_tess{};
        Shader::TessellationDataConstantBuffer tess{}; ///< read from guest memory at the lookup
        bool has_fetch{};
        std::vector<u32> fetch_code;     ///< the vertex fetch shader's words, read at the lookup
    };
    /// sotc: SOTC_PIPE_FAST. The last successful graphics lookup: its pipeline, the registers it
    /// read and its stages in bind order. Invalidated at the start of every full lookup.
    struct PipeFastGraphics {
        bool valid{};
        const GraphicsPipeline* pipeline{};
        size_t pipelines_size{};   ///< graphics_pipelines.size() when recorded
        PipeFastRegs regs{};
        Shader::Gcn::FetchShaderData* fetch{}; ///< fetch_shader as the lookup left it
        std::vector<PipeFastStage> stages;
    };
    /// sotc: SOTC_PIPE_FAST. The last successful compute lookup.
    struct PipeFastCompute {
        bool valid{};
        const ComputePipeline* pipeline{};
        size_t pipelines_size{};   ///< compute_pipelines.size() when recorded
        ComputePipelineKey key{};
        PipeFastStage stage{};
    };
    /// sotc: SOTC_PIPE_FAST. The state a lookup leaves behind, which a fast hit reproduces.
    struct PipeFastState {
        const GraphicsPipeline* pipeline{};
        const ComputePipeline* compute{};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        Shader::Gcn::FetchShaderData* fetch{};
    };
    PipeFastGraphics pipe_fast_gfx;
    PipeFastCompute pipe_fast_cs;
    /// sotc: SOTC_PIPE_FAST. Each returns true when the draw matches the record (and then fills
    /// out with the state the full lookup would leave); misses are counted by reason.
    bool GraphicsFastCheck(PipeFastState& out);
    bool ComputeFastCheck(PipeFastState& out);
    /// sotc: SOTC_PIPE_FAST. Checks one recorded stage against the draw's params: reads the guest
    /// memory the stage reads (flat buffer, tessellation constants, fetch shader) and sets its
    /// info and module in slot, as GetProgram does.
    PipeFastMiss FastVerifyStage(const PipeFastStage& st, const Shader::ShaderParams& params,
                                 size_t slot);
    /// sotc: SOTC_PIPE_FAST. Fills the record of one stage the full lookup bound. Returns false when
    /// the stage cannot be checked later.
    bool FillFastStage(PipeFastStage& st, Shader::Info& info, Shader::HwStage hw,
                       Shader::SwStage sw, const Shader::ShaderParams& params,
                       vk::ShaderModule module, Shader::Gcn::FetchShaderData* fetch);
    /// sotc: SOTC_PIPE_FAST. Records the state of a lookup that returned pipeline (after the full
    /// lookup, on the same thread). A stage that cannot be recorded leaves the record invalid.
    void RecordGraphicsFast(const GraphicsPipeline* pipeline);
    void RecordComputeFast(const ComputePipeline* pipeline);

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    /// A patch is SPIR-V edited from the module the recompiler generated on one GPU. When a
    /// "<patch>.base" file holds the XXH3 hash of that module, the patch only applies while the
    /// generated module still matches it (other GPUs/drivers can get a different module).
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext,
                                                   std::span<const u32> generated);
    /// sotc: on a pipeline worker, worker_pools replaces the command processor's pools and the
    /// SPIR-V is returned in spv_out instead of being stored (the permutation index is only
    /// known when the command processor installs the module).
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding,
                                   Shader::Pools* worker_pools = nullptr,
                                   std::vector<u32>* spv_out = nullptr);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::HwStage stage, Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

    /// sotc: SOTC_ASYNC_SHADERS (default on): a new graphics pipeline is built on a worker
    /// thread, and the draws that need it are skipped until it is ready (GetGraphicsPipeline
    /// returns nullptr meanwhile; its map entry stays null while pending). Compute dispatches are
    /// never skipped (the game reads compute results back): with SOTC_ASYNC_COMPUTE (default on)
    /// a new compute pipeline is built by a worker ahead of the graphics jobs, and the recording
    /// thread waits for it when it binds it (ComputePipeline::BindWhenReady).
    struct AsyncPipelineJob {
        /// sotc: SOTC_BLOCK_LEDGER job queue report: when the job was made (its queue wait is measured from here;
        /// for the backlogs of pretranslation and pack loads, the backlog time is included).
        u64 created_ns{SotcJobClockNs()};
        /// sotc: SOTC_NB_SCHED. Draws skipped for this graphics job (the command processor adds, the scheduler reads),
        /// and set when the command processor built the pipeline itself (the worker then skips the build).
        std::atomic<u32> demand{0};
        std::atomic<bool> cancelled{false};
        /// sotc: SOTC_NB_GPL: a link job. The worker builds the missing per-shader libraries and links them (the
        /// fast pipeline the draws use until the optimized build of the same key replaces it).
        bool link_only{};
        /// sotc: SOTC_NB_GPL (P4): a library warm-up job: a link job whose only purpose is building the new shader's
        /// library ahead of its first draw; its linked pipeline is dropped.
        bool warm_lib{};
        /// sotc: SOTC_GPL_NODROP: the libraries of a manifest pipeline (with link_only and warm_lib), built without the
        /// link (GplLibraryCache::SetLibrariesOnly); counted by the manifest hold.
        bool manifest_libs{};
        /// sotc: SOTC_GPL_PREDICT / SOTC_GPL_PARALLEL_LIBS: a library job (with link_only and warm_lib): only the parts
        /// of this mask (a bit per GplLibraryCache::Part) are built, the others only looked up; no link. lib_origin is
        /// the GplLibraryCache::Origin it counts under; lib_wait, when set, is the group a waiting draw counts down.
        u8 lib_parts{};
        u8 lib_origin{};
        /// sotc: SOTC_GPL_PACK_LIBS: the stage library this job compiles (PackLibraryId); its library is destroyed
        /// once compiled (GplLibraryCache::SetDiscard).
        u64 pack_id{};
        /// sotc: SOTC_GPL_PACK_LEAN: the program cache key and permutation of the stage a pack job compiles (its module
        /// is destroyed by the command processor once the job is done).
        u64 pack_program{};
        u32 pack_perm{};
        /// sotc: SOTC_PACK_REFERENCED: a library of a material created after the first-loading pack. Its id is listed
        /// for the next launch once it is built (PipelineCache::DrainPackRef).
        bool late_pack{};
        struct LibWait {
            std::atomic<int> remaining{0};
        };
        std::shared_ptr<LibWait> lib_wait;
        ComputePipeline* compute{}; ///< a compute job: only Build() is run
        /// sotc: SOTC_COMPUTE_FAST: a compute job of a dispatch's new pipeline, on the Urgent lane: BuildFast, then the
        /// optimized build is queued (compute_opt, Background lane) unless the driver cache had it.
        bool compute_fast{};
        bool compute_opt{}; ///< sotc: SOTC_COMPUTE_FAST: BuildOptimized (replaces the fast handle)
        bool priority{}; ///< sotc: a depth-only (shadow) graphics job queued ahead of the others
        bool pre_compute{}; ///< sotc: SOTC_PRETRANSLATE_COMPUTE: a prebuilt compute pipeline (its build counts as done)
        /// sotc: SOTC_COMPUTE_PACK: a prebuild compiled into the pack cache (the worker notes when the last one built).
        bool pack_compute{};
        /// sotc: SOTC_ADDR_FALLBACK: a table-variant pipeline queued while its draws take the descriptor pipeline
        /// (counted as pending until built).
        bool addr_table{};
        /// sotc: SOTC_PIPE_MANIFEST: a pipeline of the manifest (counted as built when collected, or by the worker
        /// for a compute pipeline). Built from its stages' recorded state, registered as a draw's.
        bool manifest{};
        GraphicsPipelineKey key;
        u64 hash{};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        GraphicsPipeline::SerializationSupport sdata{};
        std::unique_ptr<GraphicsPipeline> pipeline;
        /// sotc: a pipeline of the boot preload (SOTC_ASYNC_PRELOAD): built from the cached
        /// state, and already in the cache (not registered again).
        bool preloaded{};
        /// sotc: N5: the use count the blob was saved with (a boot prebuild job, QueueAsyncPipeline).
        u32 uses{};

        /// sotc: SOTC_ASYNC_TRANSLATE (default on): a graphics shader stage seen for the first
        /// time (or a new permutation of one) is translated by a worker, queued after the compute
        /// jobs and ahead of the pipeline jobs; the draws that need it are skipped meanwhile, as
        /// for a pipeline being built. The command processor installs the result.
        struct Translate {
            Shader::HwStage hw_stage{};
            Shader::SwStage sw_stage{};
            u64 hash{};
            u8 variant{}; ///< sotc: SOTC_HEAP stage variant (ShaderParams::variant); hash already includes it
            /// sotc: ShaderParams::guest_hash (the game's hash of a variant, 0 for the default). Without it the
            /// worker's Info took the variant-mixed hash as pgm_hash: the hash rules missed the program, and its
            /// CacheHash mixed the variant twice, so the binary (saved under hash) was never found again.
            u64 guest_hash{};
            std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
            std::span<const u32> code;
            Shader::RuntimeInfo runtime_info{};
            Shader::Backend::Bindings start{};
            Program* program{}; ///< set for a new permutation of an existing program
            size_t perm_idx{};  ///< provisional (patch file name only)
            std::optional<Shader::StageSpecialization> spec;
            std::unique_ptr<Program> new_program;
            vk::ShaderModule module{};
            std::vector<u32> spv;
            bool stale{}; ///< the worker's specialization differs from spec (permutation)
            bool failed{}; ///< translation asserted (tables reused by the game meanwhile)
            Shader::Info::BindlessCache bindless_answers{}; ///< sotc: the permutation's answers (CombineBindlessCache)
            /// sotc: SOTC_PRETRANSLATE: a translation at resource load (synthetic sharps, the file's registers); the
            /// code is the job's own copy of the shader file's program. Queued behind every other job.
            bool pretranslate{};
            std::vector<u32> owned_code;
            u8 synthetic_variant{}; ///< Info::sotc_synthetic_variant of the translation
            u8 synthetic_mask{};    ///< sotc: Info::sotc_synthetic_mask of the translation (SOTC_COMPUTE_SPECFREE)
            bool first_variant{};   ///< the job that may queue the synthetic variant 1
            bool hinted{};          ///< sotc: SOTC_COMPUTE_PACK: a compute prebuild translated with a hint (report)
            /// sotc: SOTC_PIPE_MANIFEST: the manifest stage this synthetic translation rebuilds (-1: none), the raw
            /// sharp fields of its draw, the fetch shader data of its draw (vertex), and the V# table its user data
            /// points to for the fetch attributes (ManifestUserData, filled on the worker).
            s32 manifest_stage{-1};
            SotcPretranslate::HintPtr manifest_hint;
            std::optional<Shader::Gcn::FetchShaderData> manifest_fetch;
            std::vector<u32> manifest_ud_table;
        };
        std::unique_ptr<Translate> translate; ///< a translation job

        /// sotc: SOTC_NB_IO: a program of the pretranslate pack, loaded by a worker (RunLazyLoad: pack read, meta,
        /// module) and installed by the command processor (InstallLazyLoad), instead of at its first draw.
        struct LazyLoad {
            struct Entry {
                u32 perm{};
                u64 offset{};
                u32 meta_size{};
                u32 spv_size{};
            };
            struct Perm {
                u32 idx{};
                std::unique_ptr<Program> program;
                Shader::StageSpecialization spec{};
                vk::ShaderModule module{};
            };
            u64 hash{};
            std::vector<Entry> entries; ///< every permutation of the program, from the pack index
            std::vector<Perm> perms;    ///< the ones the worker read
        };
        std::unique_ptr<LazyLoad> lazy; ///< a pack load job
    };
    /// sotc: the job lanes of the pipeline workers (JobQueues), highest first. A worker takes the first job of the
    /// first lane that has one; Background is capped (SOTC_NB_SCHED) and Graphics is ordered by demand.
    /// sotc: Urgent (SOTC_GPL_PARALLEL_LIBS): library builds a draw waits for, ahead of everything. Predict
    /// (SOTC_GPL_PREDICT): libraries of a just-created material, after the jobs a recorder or draw waits for (Compute,
    /// Translate), ahead of the optimized builds, background translations and pack loads.
    enum class JobLane : u8 { Urgent, Compute, Translate, Predict, Tiny, Shadow, Graphics, Background, Count };
    struct JobQueues {
        std::array<std::deque<std::unique_ptr<AsyncPipelineJob>>, static_cast<size_t>(JobLane::Count)> lanes;
        u32 background_running{}; ///< background jobs taken by workers and not finished
        /// sotc: jobs of the capped lanes (Shadow, Graphics, Background; CappedLane) taken by workers and not finished
        u32 capped_running{};
        /// sotc: SOTC_COMPILE_THREADS: jobs taken while a loading hold is active and not finished (any lane)
        u32 hold_running{};
        std::deque<std::unique_ptr<AsyncPipelineJob>>& operator[](JobLane lane) {
            return lanes[static_cast<size_t>(lane)];
        }
        bool Empty() const {
            for (const auto& l : lanes) {
                if (!l.empty()) {
                    return false;
                }
            }
            return true;
        }
    };
    /// sotc: queues a job in a lane (async_mutex held by the caller) and counts it for the report.
    void PushJobLocked(JobLane lane, std::unique_ptr<AsyncPipelineJob> job);
    /// sotc: the job a worker takes now (async_mutex held), or null when only capped background jobs wait.
    /// urgent_only (SOTC_GPL_URGENT_WORKERS): the Urgent and Predict lanes only.
    /// sotc: SOTC_COMPILE_THREADS: hold_slot is set when the job counts against the hold cap (HoldSlot releases it).
    std::unique_ptr<AsyncPipelineJob> PopJobLocked(JobLane& lane, bool urgent_only = false, bool* hold_slot = nullptr);
    /// sotc: SOTC_COMPILE_THREADS: the hold cap applies now (a loading hold is active and the cap is not 0).
    bool HoldCapActive() const;
    /// sotc: SOTC_COMPILE_THREADS: a loading hold starts or ends (CP). The end wakes the workers the cap held back.
    void SetCompileHold(bool active, const char* what);
    /// sotc: SOTC_COMPUTE_RELEASE (CP): destroys the modules of the queued compute permutations whose pipeline is built.
    /// force: the whole queue now (the hold ends); otherwise at most once per second.
    void ReleaseBuiltComputeModules(bool force);
    /// sotc: SOTC_COMPUTE_RELEASE: a compute pipeline exists for a permutation of the program (keyed by its cache key).
    bool ProgramHasComputePipeline(u64 program_key, const Program& program) const;
    /// sotc: SOTC_COMPILE_STREAM (CP): the released pipelines of a program whose permutations are loaded again are
    /// rebuilt from the pack cache on the workers.
    void StreamRehydrate(u64 program_hash);
    /// sotc: SOTC_COMPILE_STREAM: the counters line (every 5 s when they changed, or now with force).
    void StreamLog(bool force);
    /// sotc: a worker can take a job now (async_mutex held).
    bool HasRunnableJobLocked(bool urgent_only = false) const;
    /// sotc: SOTC_GPL_URGENT_WORKERS: a lane the reserved workers serve.
    static constexpr bool UrgentLane(JobLane lane) {
        return lane == JobLane::Urgent || lane == JobLane::Predict;
    }
    u32 BackgroundCap() const;
    /// sotc: the lanes whose jobs no draw or recorder is blocked on right now (SOTC_NB_WORKER_CAP caps them).
    static constexpr bool CappedLane(JobLane lane) {
        return lane == JobLane::Predict || lane == JobLane::Shadow || lane == JobLane::Graphics ||
               lane == JobLane::Background;
    }
    /// sotc: how many capped-lane jobs may run at once (unlimited when SOTC_NB_WORKER_CAP is off or during the boot hold).
    u32 CappedCap() const;
    bool AsyncPipelineEligible() const;
    /// sotc: front: the job goes to the front of its lane (a draw's missing pipeline, ahead of the background builds
    /// queued in the same lane: the boot preload, the manifest's late builds).
    void QueueAsyncPipeline(u64 hash,
                            const GraphicsPipeline::SerializationSupport* preloaded = nullptr,
                            bool priority = false, bool background = false, bool addr_table = false,
                            bool front = false);
    /// sotc: SOTC_ADDR_FALLBACK (default on in table mode, SOTC_SHADER_ADDR 3/4; =0 off; no effect otherwise). A draw
    /// or dispatch whose table-variant pipeline (StageVariantPlain/Heap, translated with `profile`) is not ready queues
    /// it on the workers and is recorded with the descriptor-mode pipeline instead (StageVariantDefault stages,
    /// translated with desc_profile, built by the rules a descriptor-mode run uses). The pipeline object carries the
    /// profile it was built with (Pipeline::GetProfile), and the rasterizer binds from that profile, so a fallback op
    /// binds every buffer by descriptor. A stage's profile follows from its variant (StageProfile): in table mode a
    /// default-variant stage is always a descriptor stage.
    bool AddrFallbackOn() const;
    bool AddrFallbackComputeOn() const;
    /// sotc: SOTC_ADDR_FALLBACK. The translation profile of a stage variant, and the profile of a pipeline's stages.
    const Shader::Profile& StageProfile(u8 variant) const {
        return profile.shader_addr != 0 && variant == Shader::StageVariantDefault ? desc_profile : profile;
    }
    const Shader::Profile& StagesProfile(const std::array<const Shader::Info*, MaxShaderStages>& stages) const {
        for (const auto* info : stages) {
            if (info) {
                return StageProfile(info->variant);
            }
        }
        return profile;
    }
    /// sotc: SOTC_ADDR_FALLBACK. With graphics_key / compute_key holding the table key: true when its pipeline is
    /// ready. Otherwise queues it (unless queued already) and returns false. CP only.
    bool AddrTableGraphicsReady();
    bool AddrTableComputeReady();
    bool AsyncPreloadEnabled() const;
    bool IsDepthOnlyDraw() const;
    const char* AsyncIneligibleReason() const;
    void QueueAsyncTranslate(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                             const Shader::ShaderParams& params, const Shader::RuntimeInfo& rt,
                             const Shader::Backend::Bindings& start, Program* program,
                             const Shader::StageSpecialization* spec, size_t perm_idx);
    void RunAsyncTranslate(AsyncPipelineJob::Translate& job);
    void TranslateOnWorker(AsyncPipelineJob::Translate& job, const Shader::ShaderParams& params,
                           Shader::Backend::Bindings& binding, Shader::RuntimeInfo& runtime_info,
                           Shader::Pools& worker_pools);
    void InstallTranslation(AsyncPipelineJob::Translate& job);
    /// sotc: SOTC_PRETRANSLATE_COMPUTE (CP only): queues the translation of a predicted compute program, unless it is
    /// known (warm session: dropped as the other requests are).
    void QueueComputePrebuild(SotcPretranslate::Request& request);
    /// sotc: SOTC_COMPUTE_FAST: queues the optimized build of a pipeline bound with its fast build (Background lane).
    /// Any thread (the recorder, through ComputePipeline::QueueOptimizedHook; the workers).
    void QueueComputeOptimized(ComputePipeline* pipeline);
    /// sotc: SOTC_PRETRANSLATE_COMPUTE (CP only): installs a translated compute program and queues its pipeline: the
    /// ComputePipeline is made here (its layouts are CP-only) under the key the first dispatch computes, and a worker
    /// builds the pipeline (Build, counted as done in JobsDone).
    void InstallComputePrebuild(AsyncPipelineJob::Translate& translate);
    /// sotc: SOTC_PRETRANSLATE_COMPUTE (CP only): compares a predicted compute program's registers with the first
    /// dispatch's ComputeProgram (report: verify match / mismatch, the first mismatches logged).
    void NoteComputeDispatch(const AmdGpu::ComputeProgram& cs, u64 hash);
    /// sotc: SOTC_PRETRANSLATE: queues the requests of the loaded shader files (command processor thread).
    void DrainPretranslate();
    /// sotc: the shader cache index (every program whose metas are on disk, with its permutation count): read at the
    /// end of the warm-up into lazy_programs, written when it grew (and at the end of the precompile run).
    void LoadShaderIndex();
    void SaveShaderIndex();
    /// sotc: a program of the index that no warm-up pipeline loaded: its permutations are read from the cache and
    /// their modules created now, at its first draw (command processor). False when it is not indexed.
    bool LoadLazyProgram(u64 hash);
    /// sotc: SOTC_PRETRANSLATE pack (sotc_pretranslate.pack in the variant folder). The variants translated by the
    /// boot hold (or a precompile run) are appended to one file, written sequentially, instead of two loose cache
    /// files each (~38,000 for the game, whose creation slowed the first session by ~40%). PrePackAdd stores one
    /// variant (false when no pack is being written); PrePackFinish writes its index and opens it for reading;
    /// PrePackLoad opens it at the warm-up and adds its programs to lazy_programs. LoadLazyProgram reads it first, and
    /// a program it loads from there is written to the cache as loose files too (the pipeline warm-up reads those).
    bool PrePackAdd(u64 hash, u32 perm_idx, std::vector<u8>&& meta, const std::vector<u32>& spv);
    void PrePackFinish();
    void PrePackLoad();
    /// sotc: SOTC_PRECOMPILE: progress line, and the exit once every job is done and the game stopped loading.
    void PrecompileCheck();
    /// sotc: SOTC_PRETRANSLATE: moves pre-translation jobs from pre_backlog to the workers, at most 2 at a time
    /// while the game runs (they never compete with it for the CPU), all of them while the boot hold or a precompile
    /// run waits for them.
    void FeedPretranslate();
    /// sotc: SOTC_PRETRANSLATE: once per session, in its first 2 minutes: when the shader files of the boot leave more
    /// than 300 translations (the first session, or the first after an update), the command processor holds the game
    /// on the port's loading screen with a progress bar until they are all in the cache.
    void MaybeHoldForPretranslate();
    void CollectAsyncPipelines();
    /// sotc: SOTC_NB_IO. Command processor: queues the pack load of a program the game announced (pretranslate
    /// request of a known program) when it is still in the pack. False when nothing was queued.
    bool QueueLazyPrefetch(u64 hash);
    /// sotc: SOTC_NB_IO. Worker: reads the job's permutations from the pack and creates their modules.
    void RunLazyLoad(AsyncPipelineJob::LazyLoad& load);
    /// sotc: SOTC_NB_IO. Command processor: installs a finished pack load (the permutations a draw did not load
    /// meanwhile).
    void InstallLazyLoad(AsyncPipelineJob::LazyLoad& load);
    /// sotc: installs one loaded permutation (LoadLazyProgram and InstallLazyLoad). False when it was not installed;
    /// the caller then destroys its module.
    bool InstallLazyPerm(u64 hash, AsyncPipelineJob::LazyLoad::Perm& perm);
    /// sotc: SOTC_NB_IO. Wakes the saver thread to save the driver cache now (inline when there is no saver).
    void RequestDriverCacheSave();
    /// sotc: SOTC_NB_GPL. Queues a link job for the current key (translate lane: draws wait for it).
    void QueueLinkJob();
    /// sotc: SOTC_NB_GPL (P4). Command processor: queues library warm-up jobs for new vertex and pixel permutations,
    /// using the last drawn vertex+pixel pipeline as the template for the rest of the state.
    void FeedWarmLibraries();
    /// sotc: SOTC_NB_GPL (P4). Command processor: the current draw's VS+PS state becomes the warm-up template.
    void NoteWarmTemplate();
    /// sotc: SOTC_NB_GPL. A tiny full-screen draw built its pipeline on the command processor: its key and shaders go to
    /// the list of the next boot holds (RecordTinyPipeline), which build their libraries before the first draw
    /// (BuildTinyLibraries, at the end of the hold, before the eviction).
    void RecordTinyPipeline();
    /// sotc: SOTC_PIPE_MANIFEST (gpu/sotc/pipe_manifest.h). State and steps, command processor only (the workers
    /// translate and build). LoadManifest at the first lookup after the warm-up; MaybeHoldForManifest at the start of
    /// every full lookup (the boot hold once, then ManifestPump for the stages whose code comes later);
    /// ManifestPump installs finished translations, resolves stages (a permutation in the program cache or on disk
    /// with the recorded specialization, or a synthetic translation of the loaded code) and queues the pipelines whose
    /// stages are ready; the Capture functions append what a draw or dispatch used that no manifest holds.
    struct ManifestState;
    std::unique_ptr<ManifestState> manifest;
    std::atomic<bool> warmup_done{false};  ///< sotc: WarmUp returned (the cache variant key is set)
    std::string cache_variant_key;         ///< sotc: the shader cache variant key (WarmUp), the manifest header's
    bool manifest_holding{};               ///< sotc: the manifest's boot hold is running (every worker serves it)
    void LoadManifest();
    void MaybeHoldForManifest(bool compute = false); // sotc: compute: SOTC_MANIFEST_HOLD_FAST
    void RunManifestHold();
    void ManifestPump(bool holding);
    bool ManifestResolveStage(u32 index, bool holding);
    void ManifestQueueTranslate(u32 index, const std::vector<u32>& code, bool holding);
    void ManifestInstallStage(AsyncPipelineJob::Translate& t);
    void ManifestSchedulePipes(bool holding);
    void ManifestUserData(AsyncPipelineJob::Translate& t);
    SotcPipeManifest::Header ManifestHeader() const;
    bool CaptureManifestGraphics();
    bool CaptureManifestCompute();
    /// sotc: SOTC_NB_LAZY_PSO: the pipelines used in the last sessions (id -> session of the last use), so the lazy
    /// warm-up still builds what the player is likely to meet. LoadRecentPipelines at the warm-up, NoteRecentPipeline at
    /// a pipeline's first use in the session, saved every 5 s (when changed) by a detached thread.
    void LoadRecentPipelines();
    void NoteRecentPipeline(u64 id);
    bool RecentPipeline(u64 id) const;
    void BuildTinyLibraries();
    /// sotc: SOTC_NB_IO. Moves pack loads from lazy_backlog to the workers, at most 2 at a time.
    void FeedLazyPrefetch();
    /// sotc: SOTC_PIPE_PREDICT (gpu/sotc/pipe_predict.h). Command processor, before a draw's key is built: reads the
    /// queued materials and queues the pipelines of their pass slots whose pass template is known.
    void PredictPipelines();
    /// sotc: SOTC_PIPE_PREDICT. After a draw's key is looked up: learns the pass template from a draw of a known
    /// material slot, and counts the draws that found a predicted pipeline.
    void NotePredictDraw(bool is_new, bool built);
    /// sotc: N5 (SOTC_N5_PSO=1). Waits for the graphics pipelines of the boot prebuild (queued by
    /// WarmUp), collecting them as they are built, up to SOTC_N5_PSO_WAIT_S seconds from start.
    /// The rest is built on the workers while the game runs.
    void WaitPreloadBoot(std::chrono::steady_clock::time_point start);
    /// sotc: N5. Rewrites the use count of the pipeline blobs whose count changed this session.
    /// Called from the destructor, as the driver cache is.
    void SaveUseCounts();
    /// sotc: SOTC_PIPE_GPL. Builds the fast-linked pipeline of graphics_key into slot, when its
    /// state can be linked. Returns false (slot untouched) when it cannot. Command processor only.
    /// sotc: probe (SOTC_GPL_PARALLEL_LIBS): links only libraries that exist, builds none.
    bool TryFastLink(std::unique_ptr<GraphicsPipeline>& slot, bool allow_build = false, bool probe = false);
    /// sotc: SOTC_GPL_NODROP: whether this draw's pipeline can be fast-linked (and later replaced by its optimized build
    /// on a worker), and the link itself, building missing libraries on this thread (counted and timed).
    bool NoDropLinkable();
    bool NoDropLink(std::unique_ptr<GraphicsPipeline>& slot);
    /// sotc: SOTC_GPL_PARALLEL_LIBS: the missing vertex and pixel libraries of the current draw (Missing() bits of a
    /// lookup-only link), built at once by two urgent jobs while the command processor waits (taking a job no worker
    /// has taken yet). False when nothing was queued (the caller builds them itself).
    bool BuildLibrariesParallel(u32 missing);
    /// sotc: a library job for the current stage state (infos, modules, runtime infos, fetch shader) under key.
    std::unique_ptr<AsyncPipelineJob> MakeLibraryJob(const GraphicsPipelineKey& key, u32 parts, u8 origin);
    /// sotc: runs a library job on this thread (a worker, or the command processor taking a waited one) and counts it
    /// done (lib_jobs_pending, its wait group).
    void RunLibraryJob(AsyncPipelineJob& job);
    /// sotc: SOTC_GPL_DYN_STATE / SOTC_GPL_PACK_LIBS. A library job that builds only `parts` of one stage permutation
    /// (vs or ps), the other stage being a partner whose libraries are only looked up. Null when the pair cannot be
    /// linked. With shader-only library keys the result does not depend on rest or on the partner.
    std::unique_ptr<AsyncPipelineJob> MakeStageLibraryJob(const GraphicsPipelineKey& rest, Program& vs, size_t vs_perm,
                                                          Program& ps, size_t ps_perm, u32 parts, u8 origin);
    /// sotc: SOTC_GPL_PACK_LIBS (sotc/nonblocking.h). At the end of the pretranslate hold, before the eviction: compiles
    /// the stage library of every translated vertex/pixel permutation that sotc_pack_libraries.bin does not list into
    /// the driver's pipeline cache (destroyed after), saves that cache and the list. Command processor.
    bool BuildPackLibraries(); // sotc: false on incomplete jobs/builds/persistence; logged skips return true
    bool PackLibraryListMatches() const;
    /// sotc: SOTC_COMPUTE_PACK (sotc/nonblocking.h). Inside BuildPackLibraries, before the publication: every compute
    /// prebuild of the hold (compiled into the pack cache) is counted, its id (PackComputeId) appended to `compiled`,
    /// and its pipeline and module destroyed unless a dispatch bound it; the program goes back to the index at the
    /// eviction. Logs the "SOTC COMPUTE PACK" line. Command processor.
    void PackComputePipelines(const std::unordered_set<u64>& listed, std::vector<u64>& compiled,
                              std::chrono::steady_clock::time_point start);
    /// sotc: SOTC_COMPUTE_PACK: the VkPipelineCache the compute pack builds into (the pack cache when the caches are
    /// split, else the driver cache), used again by a dispatch whose pipeline the list names.
    vk::PipelineCache PackPipelineCache() const;
    /// sotc: SOTC_COMPUTE_PACK: whether the pack list (loaded once, lazily, when valid) names this compute key's
    /// pipeline; such a dispatch builds inline on the recorder (no worker job). Command processor.
    bool PackComputeListed(u64 compute_key_value);
    /// sotc: SOTC_COMPUTE_PACK state (CP only): the listed ids as a set (loaded from the file or from the publication),
    /// the prebuilds translated with a hint, the queued/capped prebuild counts, and the first install / last build
    /// times (the "in T s" of the pack line).
    std::unordered_set<u64> pack_listed_set;
    bool pack_listed_set_loaded{};
    std::unordered_set<u64> pack_compute_hinted;
    u64 pack_compute_queued{}, pack_compute_capped{};
    std::atomic<s64> pack_compute_first_ns{0}, pack_compute_last_ns{0};
    std::atomic<u64> pack_compute_build_ns{0}; ///< worker time of the pack's compute builds
    bool pack_list_checked{}, pack_list_valid{}; ///< sotc: startup validity, independent of translation backlog
    /// sotc: SOTC_GPL_PACK_LIBS: ids of the pack libraries compiled this session (workers append).
    std::mutex pack_mutex;
    std::vector<u64> pack_compiled;
    std::atomic<u64> pack_failed{0};
    /// sotc: SOTC_GPL_PACK_LEAN: (program cache key, permutation) of the pack jobs done (workers append under pack_mutex;
    /// the command processor destroys those modules while the hold runs) and the programs whose modules it destroyed.
    std::vector<std::pair<u64, u32>> pack_done_modules;
    std::unordered_set<u64> pack_touched;
    /// sotc: SOTC_GPL_PACK_LEAN: the workers added for the pack compile (up to the hardware thread count), stopped
    /// after it (they leave at the next idle moment, without draining the queues).
    std::vector<std::jthread> extra_workers;
    std::atomic<bool> extra_workers_quit{false};
    void StartExtraWorkers();
    void StopExtraWorkers();
    /// sotc: SOTC_GPL_PACK_LEAN: the pack publication (the driver cache, then the list of the ids below) on the saver
    /// thread. state: 0 idle, 1 requested, 2 written, 3 failed; pack_publish_cv (driver_cache_cv_mutex) on 2 or 3.
    std::vector<u64> pack_publish_ids;
    std::vector<u8> pack_publish_header;
    std::atomic<u32> pack_publish_state{0};
    std::condition_variable_any pack_publish_cv;
    void RunPackPublish();
    /// sotc: SOTC_GPL_PACK_LIBS: the saved driver cache was loaded and accepted at startup (else the list is void).
    bool driver_cache_loaded{};
    u64 loaded_driver_cache_hash{}; ///< sotc: immutable identity of the startup cache used by the pack list
    /// sotc: SOTC_GPL_LIB_PHASE: queues the manifest library jobs held back for the library phase of the boot hold.
    void ManifestFlushLibraries(JobLane lane);
    /// sotc: SOTC_GPL_PREDICT (gpu/sotc/pipe_predict.h). Command processor, before a draw's key is built: takes the
    /// queued materials' pass slots (or the ones PredictPipelines read) and queues the library jobs of their programs.
    void PredictLibraries(const std::vector<SotcPipePredict::Slot>* slots);
    /// sotc: SOTC_GPL_PREDICT. After a new key: learns the pre-rasterization and fragment state of its pass slot and
    /// counts the most common ones (the templates of the predicted library jobs).
    void NoteLibraryTemplate();
    /// sotc: SOTC_GPL_PREDICT state (vk_pipeline_cache.cpp), command processor only.
    std::shared_ptr<struct LibPredictState> lib_predict;
    /// sotc: SOTC_GPL_PREDICT / SOTC_GPL_PARALLEL_LIBS library jobs queued and not finished (workers decrement).
    std::atomic<u32> lib_jobs_pending{0};
    /// sotc: SOTC_GPL_PARALLEL_LIBS: notified (under async_mutex) when a waited library job is done.
    std::condition_variable_any lib_wait_cv;
    /// sotc: SOTC_GPL_URGENT_WORKERS: the reserved workers wait on it (async_mutex); a push to the Urgent or Predict lane
    /// notifies it (PushJobLocked, BuildLibrariesParallel).
    std::condition_variable_any urgent_cv;
    /// sotc: SOTC_ASYNC_COMPUTE_XLATE: waits for (or takes back) a prefetched translation.
    void WaitPendingTranslation(u64 hash);
    /// sotc: SOTC_SHADER_ADDR (table mode). A compute translation the command processor would make inline (a new
    /// program, or a new permutation of `program` for `spec`) is queued on the Urgent lane and waited for here, so
    /// the SPIR-V emit and the driver's module compile run on a worker. True when the translation was installed (the
    /// lookup then finds it); false when it does not apply (descriptor mode, no workers) or timed out. CP only.
    bool TranslateComputeOnWorker(const Shader::ShaderParams& params, const Shader::RuntimeInfo& rt,
                                  const Shader::Backend::Bindings& start, Program* program,
                                  const Shader::StageSpecialization* spec, size_t perm_idx);
    /// sotc: urgent_only (SOTC_GPL_URGENT_WORKERS): serves the Urgent and Predict lanes only. extra (SOTC_GPL_PACK_LEAN):
    /// a worker of the pack hold, which leaves when extra_workers_quit is set.
    void AsyncPipelineWorker(std::stop_token token, bool urgent_only = false, bool extra = false);

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    // sotc: shared pipeline layouts. Declared before the pipelines, so they outlive them.
    LayoutCache layout_cache;
    /// sotc: SOTC_PIPE_GPL: the pipeline libraries of the fast-linked pipelines. Declared after the
    /// layouts, so it is destroyed before them.
    GplLibraryCache gpl_libs;
    vk::UniquePipelineCache pipeline_cache;
    size_t saved_driver_cache_size{};
    /// sotc: SOTC_PLAY_QUIET: the second VkPipelineCache, for the stage libraries (pack, predicted, manifest, urgent;
    /// GplLibraryCache::SetLibraryCache). Loaded from sotc_vk_pack.bin, written by the pack publication (RunPackPublish)
    /// and at exit when it grew (FinalPackPublish); the periodic saver never reads it. Null when the switch is off.
    vk::UniquePipelineCache pack_cache;
    size_t saved_pack_cache_size{};
    u64 pack_cache_published{}; ///< GplLibraryCache::LibraryBuilds at the last pack publication
    std::vector<u64> pack_listed_ids; ///< the ids of the last list written or read (driver_cache_cv_mutex)
    void FinalPackPublish();
    /// sotc: SOTC_PACK_REFERENCED. The first-loading pack ran (materials created later are late), its figures for the
    /// log line, and the late libraries: queued, built, and whether a built one came before its program's first draw.
    struct PackRef {
        struct Done {
            u64 id{};
            u64 program{};
            bool ok{};
        };
        bool pack_done{};
        u64 materials{}, programs{}, programs_all{}, libs_at_load{};
        double load_seconds{};
        u64 queued{}, built{}, before{}, after{};
        std::unordered_map<u64, bool> watch; ///< program hash of a late library -> drawn yet (command processor)
        u64 undrawn{};                       ///< watch entries not drawn yet (command processor)
        std::unordered_set<u64> listed_late; ///< late ids appended to pack_listed_ids (command processor)
        std::atomic<u32> done_count{};       ///< entries of done not drained yet
        std::vector<Done> done;              ///< pack_mutex: late libraries finished on a worker
    };
    PackRef pack_ref;
    void DrainPackRef();      ///< command processor: counts the late libraries finished, and lists them
    void NotePackRefDraw();   ///< command processor: a draw's programs (the first draw of a late library's program)
    void PackReferencedLog(); ///< the SOTC PACK REFERENCED line
    /// sotc: one VkPipelineCache to one file, whole, through a temporary (driver_cache_save_mutex). final_save skips the
    /// growth threshold of the periodic saves.
    bool SaveCacheFile(vk::PipelineCache cache, const std::filesystem::path& path, size_t& saved_size, bool final_save,
                       size_t* written_size, u64* written_hash, const char* what);
    std::mutex driver_cache_save_mutex;
    /// sotc: SOTC_NB_IO: RequestDriverCacheSave wakes the saver (declared before it: it outlives the thread).
    std::mutex driver_cache_cv_mutex;
    std::condition_variable_any driver_cache_cv;
    bool driver_cache_save_requested{};
    std::jthread driver_cache_saver;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    /// sotc: SOTC_ADDR_FALLBACK: `profile` without the shader addresses (shader_addr 0, gcn_buf 0): the profile of a
    /// descriptor-mode run. Declared after `profile` and before the pipelines (which keep a reference to it).
    Shader::Profile desc_profile{};
    bool addr_fallback{};      ///< sotc: SOTC_ADDR_FALLBACK resolved (table mode and the switch not 0)
    bool addr_fallback_draw{}; ///< sotc: the current lookup binds the descriptor-mode stages (CP only)
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
    /// sotc: SOTC_VS_DRAW_KIND: the vertex stage's user data of an indirect draw, with the start instance SGPR zeroed.
    std::array<u32, Shader::ShaderParams::NumShaderUserData> vs_draw_kind_user_data{};
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    // sotc: any erase or clear must also null last_graphics_pipeline (see GetGraphicsPipeline).
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    GraphicsPipelineKey last_graphics_key{};
    const GraphicsPipeline* last_graphics_pipeline{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    Shader::Gcn::FetchShaderData* fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    /// sotc: SOTC_PIPE_PREDICT state (vk_pipeline_cache.cpp), command processor only; null with the switch off.
    std::shared_ptr<struct PipePredictState> pipe_predict;
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    // sotc: SOTC_ASYNC_SHADERS state (see AsyncPipelineJob). The workers are declared last so
    // they are stopped and joined before anything they use is destroyed.
    std::mutex async_mutex;
    std::condition_variable_any async_cv;
    JobQueues job_queues; ///< sotc: the workers' job lanes (async_mutex)
    /// sotc: SOTC_NB_SCHED (CP only): queued graphics jobs by key, for the demand of skipped draws and the cancel of
    /// a pipeline the command processor built itself. Entries leave when the job is collected.
    std::unordered_map<GraphicsPipelineKey, AsyncPipelineJob*> queued_graphics;
    std::unordered_set<GraphicsPipelineKey> link_pending; ///< sotc: SOTC_NB_GPL link jobs not collected (CP only)
    /// sotc: SOTC_NB_GPL (P4) warm-up state (CP only).
    struct WarmTemplate {
        bool valid{};
        GraphicsPipelineKey key{};
        u64 vs_hash{}, ps_hash{};
        size_t vs_perm{}, ps_perm{};
    } warm_template;
    std::vector<std::pair<u64, size_t>> warm_log;        ///< new permutations not warmed yet (Program::new_perm_log)
    std::unordered_set<u64> warm_done;                    ///< HashCombine(hash, perm) already queued
    u64 warm_queued{}, warm_built{}, warm_skipped{};
    /// sotc: SOTC_NB_GPL tiny full-screen pipelines (CP only): key -> its vertex and pixel program and permutation.
    struct TinyEntry {
        u64 vs_hash{}, ps_hash{};
        u32 vs_perm{}, ps_perm{};
    };
    std::unordered_map<GraphicsPipelineKey, TinyEntry> tiny_list;
    std::unordered_map<u64, u32> recent_pso;
    u32 recent_session{};
    bool recent_dirty{};
    std::chrono::steady_clock::time_point recent_saved{};
    bool tiny_list_loaded{};
    /// sotc: compute pipelines of the boot preload built by the workers (SOTC_ASYNC_PRELOAD);
    /// WarmUp waits for them and drops the ones the driver refused, as it does when it builds
    /// them itself.
    /// sotc: N5: the use count of the blob WarmUp is loading (0 outside the boot prebuild).
    u32 warm_uses{};
    std::vector<ComputePipelineKey> preload_computes;
    /// sotc: preloaded compute pipelines the driver refused, out of the map but kept alive: their
    /// worker job may still be queued (it returns at once, the pipeline being claimed).
    std::vector<std::unique_ptr<ComputePipeline>> preload_refused;

    std::unordered_set<u64> pending_programs; ///< new programs being translated (CP only)
    std::unordered_set<u64> in_place_programs; ///< failed on a worker: translated here (CP only)
    /// sotc: SOTC_PRETRANSLATE (CP only): programs whose pending translation is a pre-translation (a draw that needs
    /// one queues its own translation), and vertex requests that wait for their pixel program.
    std::unordered_set<u64> pre_pending;
    /// sotc: programs of the shader cache index not loaded yet (CP only): hash -> permutations on disk.
    std::unordered_map<u64, u32> lazy_programs;
    /// sotc: SOTC_NB_IO (CP only): programs whose pack load is queued or running on a worker.
    std::unordered_set<u64> lazy_prefetch;
    std::deque<std::unique_ptr<AsyncPipelineJob>> lazy_backlog; ///< sotc: SOTC_NB_IO pack loads not on a worker yet
    u32 lazy_in_flight{};                                       ///< sotc: SOTC_NB_IO pack loads on the workers
    std::deque<std::unique_ptr<AsyncPipelineJob>> pre_backlog; ///< sotc: pre-translation jobs not given to workers
    u32 pre_in_flight{};                                       ///< sotc: pre-translation jobs on the workers
    bool pre_holding{};                                        ///< sotc: the boot hold is waiting for them
    bool pre_hold_done{};                                      ///< sotc: the hold ran (once per session)
    std::vector<SotcPretranslate::Request> pre_deferred;
    /// sotc: SOTC_PRETRANSLATE_COMPUTE (CP only): the predicted registers of the prebuilt compute programs, until their
    /// first dispatch (NoteComputeDispatch) has compared them.
    struct PreComputePrediction {
        u32 rsrc1{}, rsrc2{}, nx{}, ny{}, nz{};
        std::array<u32, 16> raw{};
        u8 layout{};
    };
    std::unordered_map<u64, PreComputePrediction> pre_compute_pred;
    /// sotc: compute pipeline builds queued by InstallComputePrebuild and not yet built (workers decrement).
    std::atomic<u32> pre_builds_pending{};
    std::chrono::steady_clock::time_point pre_deferred_since{};
    /// sotc: SOTC_HEAP. Heap candidates (the variant's hash) that Heap::StageRefused refused, kept in the cache folder
    /// ("heap_refused" blob, loaded by WarmUp). A refused candidate's program is in no saved pipeline, so without
    /// this list it was translated again at every start (~850 shaders per session) only to be refused again.
    /// CP only (RefreshGraphicsStages, RefreshComputeKey).
    std::unordered_set<u64> heap_refused;
    void NoteHeapRefused(u64 candidate_hash);
    bool async_translate_ok{};  ///< this draw may queue its translations (CP only)
    bool translation_pending{}; ///< a stage of this draw is being translated (CP only)
    std::vector<std::unique_ptr<AsyncPipelineJob>> async_done;
    std::atomic<u32> async_done_count{0};
    /// sotc: notified (under async_mutex) when a translation job is done.
    std::condition_variable_any translate_done_cv;
    bool compute_prefetch{}; ///< SOTC_ASYNC_COMPUTE_XLATE=1 and workers exist
    /// sotc: SOTC_COMPILE_THREADS: loading holds in progress (pretranslate, manifest), set by SetCompileHold on the CP
    /// and read by the workers under async_mutex.
    std::atomic<int> compile_holds{0};
    /// sotc: SOTC_COMPUTE_RELEASE (CP only): compute permutations whose pipeline was created and whose module may be
    /// destroyed once it is built: program cache key, permutation, pipeline key.
    struct ComputeRelease {
        u64 program{};
        u32 perm{};
        u64 key{};
    };
    std::vector<ComputeRelease> compute_release_queue;
    std::chrono::steady_clock::time_point compute_release_last{};
    u64 compute_modules_released{};
    /// sotc: SOTC_COMPILE_STREAM (CP only): pipeline keys the pack step destroyed, and their programs; the counters of
    /// the "SOTC COMPILE STREAM" line (rebuilt on workers, built by a dispatch first, dispatched before the rebuild was
    /// ready).
    std::unordered_set<u64> stream_released_keys;
    std::unordered_set<u64> stream_released_programs;
    u64 stream_released{}, stream_rehydrated{}, stream_inline{}, stream_late{};
    std::chrono::steady_clock::time_point stream_logged{};
    std::vector<std::jthread> async_workers;
    unsigned reserved_workers{}; ///< sotc: SOTC_GPL_URGENT_WORKERS: of them, the Urgent/Predict-only ones
};

/// sotc: SOTC_PIPE_MEMO=1/2 counters (vk_pipeline_cache.cpp): appends one line for the 5 s report
/// when the switch is on and the window had lookups. Call from one thread only.
void ReportPipeMemo(std::string& out, double seconds);

/// sotc: SOTC_SPEC_MEMO=1/2 counters (vk_pipeline_cache.cpp): appends one "SOTC spec memo" line for
/// the 5 s report when the switch is on and the window had lookups. Call from one thread only.
void ReportSpecMemo(std::string& out, double seconds);

/// sotc: SOTC_PIPE_DIRTY=1/2 counters (vk_pipeline_cache.cpp): appends one line for the 5 s report
/// when the switch is on and the window had graphics key or flat buffer work. Call from one thread.
void ReportPipeDirty(std::string& out, double seconds);

/// sotc: SOTC_PIPE_FAST counters (vk_pipeline_cache.cpp): appends one line for the 5 s report when
/// the switch is on and the window had lookups. Call from one thread only.
void ReportPipeFast(std::string& out, double seconds);

/// sotc: SOTC_PIPE_GPL counters (vk_pipeline_cache.cpp): appends one line for the 5 s report when the
/// switch is on and the window had cold misses or libraries. Call from one thread.
void ReportPipeGpl(std::string& out, double seconds);

/// sotc: SOTC_PIPE_MANIFEST (vk_pipeline_cache.cpp): writes the records captured since the last call to the user's
/// manifest, and appends one "SOTC PIPE MANIFEST late" line when pipelines were built after the boot hold in the
/// window. Call from one thread.
void ReportPipeManifest(std::string& out, double seconds);

/// sotc: SOTC_ADDR_FALLBACK counters (vk_pipeline_cache.cpp): appends one "SOTC ADDR FALLBACK" line for the 5 s report
/// when the fallback is on and the window had fallback ops or table pipelines pending/built. Call from one thread.
void ReportAddrFallback(std::string& out, double seconds);

/// sotc: SOTC_COMPUTE_FAST counters (vk_compute_pipeline.cpp): appends one "SOTC COMPUTE FAST (5 s)" line when the
/// switch is on and the window had fast builds, swaps or waits. Call from one thread.
void ReportComputeFast(std::string& out, double seconds);
/// sotc: SOTC_COMPUTE_SPECFREE, the 5 s report line (vk_compute_pipeline.cpp; prints only when on).
void ReportComputeSpecFree(std::string& out, double seconds);

/// sotc: N5 (SOTC_N5_PSO=1 with SOTC_NATIVE on). The boot prebuild in WarmUp: every cached pipeline
/// is built on the workers before the game needs it, in the order of its use count.
bool N5PsoEnabled();
/// sotc: N5. WarmUp brackets its work with this: its compiles (and the boot wait) are not stalls.
void N5SetWarming(bool warming);
/// sotc: N5. The use count stored in a pipeline key blob (0 when the blob has none: a cache written
/// before N5, or with N5 off).
u32 PipelineUseCount(const std::vector<u8>& blob);
/// sotc: N5. Sets the use count of the pipeline key blob name (without extension) to uses.
bool RewritePipelineUseCount(const std::string& name, u32 uses);
/// sotc: N5 counters: boot prebuild progress and the sync compiles (command processor or draw
/// thread) after SOTC_N5_PSO_GRACE_S seconds, per window. Appends one line each; call from one thread.
void ReportN5Pso(std::string& out, double seconds);

} // namespace Vulkan
