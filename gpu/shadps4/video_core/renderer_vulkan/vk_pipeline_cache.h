// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
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

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
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

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
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
    void SaveDriverCache();
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
        return program_cache.contains(hash) || pending_programs.contains(hash) ||
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
        ComputePipeline* compute{}; ///< a compute job: only Build() is run
        bool priority{}; ///< sotc: a depth-only (shadow) graphics job queued ahead of the others
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
        };
        std::unique_ptr<Translate> translate; ///< a translation job
    };
    bool AsyncPipelineEligible() const;
    void QueueAsyncPipeline(u64 hash,
                            const GraphicsPipeline::SerializationSupport* preloaded = nullptr,
                            bool priority = false);
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
    void CollectAsyncPipelines();
    /// sotc: N5 (SOTC_N5_PSO=1). Waits for the graphics pipelines of the boot prebuild (queued by
    /// WarmUp), collecting them as they are built, up to SOTC_N5_PSO_WAIT_S seconds from start.
    /// The rest is built on the workers while the game runs.
    void WaitPreloadBoot(std::chrono::steady_clock::time_point start);
    /// sotc: N5. Rewrites the use count of the pipeline blobs whose count changed this session.
    /// Called from the destructor, as the driver cache is.
    void SaveUseCounts();
    /// sotc: SOTC_PIPE_GPL. Builds the fast-linked pipeline of graphics_key into slot, when its
    /// state can be linked. Returns false (slot untouched) when it cannot. Command processor only.
    bool TryFastLink(std::unique_ptr<GraphicsPipeline>& slot);
    /// sotc: SOTC_ASYNC_COMPUTE_XLATE: waits for (or takes back) a prefetched translation.
    void WaitPendingTranslation(u64 hash);
    void AsyncPipelineWorker(std::stop_token token);

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
    std::mutex driver_cache_save_mutex;
    std::jthread driver_cache_saver;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
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
    std::deque<std::unique_ptr<AsyncPipelineJob>> async_queue;
    size_t async_compute_queued{}; ///< compute jobs at the head of async_queue, oldest first
    /// sotc: compute pipelines of the boot preload built by the workers (SOTC_ASYNC_PRELOAD);
    /// WarmUp waits for them and drops the ones the driver refused, as it does when it builds
    /// them itself.
    /// sotc: N5: the use count of the blob WarmUp is loading (0 outside the boot prebuild).
    u32 warm_uses{};
    std::vector<ComputePipelineKey> preload_computes;
    /// sotc: preloaded compute pipelines the driver refused, out of the map but kept alive: their
    /// worker job may still be queued (it returns at once, the pipeline being claimed).
    std::vector<std::unique_ptr<ComputePipeline>> preload_refused;
    size_t async_translate_queued{}; ///< translation jobs right after them, oldest first
    size_t async_priority_queued{}; ///< sotc: shadow graphics jobs right after those, oldest first
    std::unordered_set<u64> pending_programs; ///< new programs being translated (CP only)
    std::unordered_set<u64> in_place_programs; ///< failed on a worker: translated here (CP only)
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
    std::vector<std::jthread> async_workers;
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
