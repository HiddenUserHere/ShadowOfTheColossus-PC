// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <ranges>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include "bbport_toggles.h"
#include "common/elf_info.h"
#include "common/assert_scope.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/serdes.h" // sotc: SOTC_PIPE_MANIFEST (specialization bytes)
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h" // sotc: PassthroughSensitiveInputs
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_bda_table.h" // sotc: SOTC_BDA profile flag
#include "video_core/sotc_arch/sotc_arch.h"           // sotc: SOTC_HEAP, SOTC_SHADER_ADDR profile flags
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_frame_metrics.h" // sotc: SOTC_FRAME_METRICS
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE (SOTC_PIPE_DIRTY default, N5)
#include "../../../sotc/predict_probe.h" // sotc: SOTC_PRETRANSLATE (pixel layouts)
#include "../../../sotc/pipe_predict.h"  // sotc: SOTC_PIPE_PREDICT
#include "../../../sotc/block_ledger.h" // sotc: SOTC_BLOCK_LEDGER
#include "../../../sotc/driver_cache_identity.h" // sotc: do not trust silently ignored initial data
#include "../../../sotc/preparation_status.h" // sotc: wait for compute builds, allow fully cached translations
#include "../../../sotc/manifest_capture.h" // sotc: raw hints must describe the recorded module specialization
#include "../../../sotc/nonblocking.h"  // sotc: SOTC_NB_* (non-blocking frame)
#include "bbport_overlay.h"               // sotc: SOTC_PLAY_QUIET (BbOverlay::LoadingShown)
#include "video_core/renderer_vulkan/sotc_pipe_feedback.h" // sotc: SOTC_PLAY_QUIET (LastCreateNs)
#include "video_core/sotc_arch/addr_table.h" // sotc: ARCH-GCN-NATIVE (Init before the profile)
#include "video_core/sotc_arch/gcn_native.h" // sotc: ARCH-GCN-NATIVE (Profile::gcn_buf, gcn_tex)
#include "bbport_threads.h"
#ifdef _WIN32
#include <psapi.h> // sotc: SOTC RAM lines (GetProcessMemoryInfo)
#endif

// sotc: generation of the runtime memory table (src/runtime_memory.c); odd while a change is in
// progress. Same declaration as vk_rasterizer.cpp and sotc_bininfo.cpp.
extern "C" uint64_t runtime_memory_generation(void);

namespace Vulkan {

namespace {
// sotc: SOTC_COMPILE_THREADS. The physical cores of the machine (GetLogicalProcessorInformation), else half the
// hardware threads.
unsigned PhysicalCoreCount() {
#ifdef _WIN32
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    if (bytes != 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) +
                                                               1);
        bytes = static_cast<DWORD>(info.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(info.data(), &bytes)) {
            unsigned cores = 0;
            const size_t n = bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            for (size_t i = 0; i < n; ++i) {
                cores += info[i].Relationship == RelationProcessorCore ? 1u : 0u;
            }
            if (cores != 0) {
                return cores;
            }
        }
    }
#endif
    return std::max(1u, std::thread::hardware_concurrency() / 2);
}

// sotc: SOTC_COMPILE_THREADS=<n> (default: the physical cores, at least 2; 0: off, the former behaviour). While a
// loading hold is active (pretranslate hold with its pack step and compute prebuilds, manifest hold), at most n jobs
// run on the pipeline workers at once, below normal priority; the extra pack workers are not started beyond n. Play
// keeps the normal worker count and priority. Read once.
unsigned CompileThreadCap() {
    static const unsigned n = [] {
        const char* v = std::getenv("SOTC_COMPILE_THREADS");
        if (v != nullptr && v[0] != '\0') {
            const long parsed = std::strtol(v, nullptr, 10);
            return parsed <= 0 ? 0u : static_cast<unsigned>(parsed);
        }
        return std::max(2u, PhysicalCoreCount());
    }();
    return n;
}

// sotc: "SOTC RAM <where>: private X MB, working set Y MB" (PrivateUsage and WorkingSetSize of the process), at the
// cache load and at the start and end of each loading hold, so a cold run shows where the memory grows.
void LogRam(const char* where) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        std::fprintf(stderr, "SOTC RAM %s: private %llu MB, working set %llu MB\n", where,
                     static_cast<unsigned long long>(counters.PrivateUsage >> 20),
                     static_cast<unsigned long long>(counters.WorkingSetSize >> 20));
    }
#else
    (void)where;
#endif
}

// sotc: SOTC_CACHE_MAP. The bytes of a saved pipeline cache file: a read-only view of the file (mapped), or the
// former vector read (SOTC_CACHE_MAP=0, or the mapping failed). Release drops either right after vkCreatePipelineCache.
class CacheFileBytes {
public:
    CacheFileBytes() = default;
    CacheFileBytes(const CacheFileBytes&) = delete;
    CacheFileBytes& operator=(const CacheFileBytes&) = delete;
    ~CacheFileBytes() {
        Release();
    }
    std::vector<u8> data;
    bool Empty() const {
        return Size() == 0;
    }
    size_t Size() const {
        return view ? view_size : data.size();
    }
    const u8* Data() const {
        return view ? view : data.data();
    }
    /// The whole file mapped read-only; false (nothing held) on failure.
    bool Map(const std::filesystem::path& path, size_t bytes) {
#ifdef _WIN32
        Release();
        if (bytes == 0) {
            return false;
        }
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            file = nullptr;
            return false;
        }
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping) {
            view = static_cast<const u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bytes));
        }
        if (!view) {
            Release();
            return false;
        }
        view_size = bytes;
        return true;
#else
        (void)path;
        (void)bytes;
        return false;
#endif
    }
    void Release() {
        data.clear();
        data.shrink_to_fit();
#ifdef _WIN32
        if (view) {
            UnmapViewOfFile(view);
        }
        if (mapping) {
            CloseHandle(mapping);
        }
        if (file) {
            CloseHandle(file);
        }
        file = nullptr;
        mapping = nullptr;
#endif
        view = nullptr;
        view_size = 0;
    }

private:
#ifdef _WIN32
    HANDLE file{};
    HANDLE mapping{};
#endif
    const u8* view{};
    size_t view_size{};
};
} // namespace

// sotc: SOTC_PIPE_MANIFEST state (gpu/sotc/pipe_manifest.h; the steps are at the end of this file). Command processor
// only, except the counters the workers and the report read (g_manifest_stats).
struct PipelineCache::ManifestState {
    enum class St : u8 { Pending, Waiting, Queued, Ready, Failed };
    struct StageRt {
        SotcPipeManifest::Stage rec;
        Shader::StageSpecialization spec; ///< the draw's specialization (info set to the program's when compared)
        SotcPretranslate::HintPtr hint;
        u64 cache_hash{};                 ///< the program cache key (the variant mixed in)
        St state{St::Pending};
        bool late{};                      ///< its code did not come with an earlier boot load: the hold does not wait
        Program* program{};
        u32 perm{};
        const char* reason{};             ///< why it failed
    };
    enum class Ps : u8 { Waiting, Queued, Done, Skipped };
    struct PipeRt {
        bool compute{};
        GraphicsPipelineKey key{};             ///< stage hashes zero (set from the stages' permutations)
        std::vector<std::pair<u8, u32>> stages; ///< (slot, index in stages)
        Ps state{Ps::Waiting};
    };
    SotcPipeManifest::Header header;
    std::vector<StageRt> stages;
    std::vector<PipeRt> pipes;
    /// Translations the workers finished, installed by the next pump (never in the middle of a lookup).
    std::vector<std::unique_ptr<AsyncPipelineJob::Translate>> finished;
    u32 translating{};       ///< translations queued and not installed
    u32 graphics_building{}; ///< graphics builds queued and not collected
    u32 libs_building{};     ///< sotc: SOTC_GPL_NODROP library jobs queued and not collected
    u64 libs_queued{};       ///< sotc: SOTC_GPL_NODROP library jobs queued in all
    /// sotc: SOTC_GPL_LIB_PHASE: the library jobs of the first boot window, queued by the hold once the translations and
    /// pipelines are done (ManifestFlushLibraries), and the process private bytes at that point (0: not sampled).
    std::vector<std::unique_ptr<AsyncPipelineJob>> lib_backlog;
    bool lib_phase_done{};
    u64 private_mid{};
    u32 start_rebased{};     ///< sotc: SOTC_STAGE_BINDINGS: stage records whose start binding was read as 0
    u64 compute_queued{};    ///< compute builds queued (g_manifest_stats.compute_built counts the built ones)
    u64 seen_arrivals{~u64{0}};
    bool dirty{true};        ///< a stage changed since the last pass over the pipelines
    bool hold_done{};        ///< the boot window is over
    bool held_once{};        ///< the first boot hold ran (later holds only for a burst of new code)
    bool boot_lanes{true};   ///< jobs go to the boot lanes, uncapped (else background, two translations at a time)
    std::chrono::steady_clock::time_point loaded{}, window_start{}, hold_start{}, last_pump{}, last_progress{};
    std::chrono::steady_clock::duration held{}; ///< the boot holds' time so far
    u64 built_graphics{}, refused{}, from_disk{};
    u64 saved_built{}; ///< pipelines built when the driver cache was last saved
    u32 stage_cached{}, stage_translated{};
    std::map<std::string, u32> skipped; ///< reason -> pipelines skipped
    // sotc: SOTC_MANIFEST_HOLD_FAST: stages in the Pending and Waiting states, kept by SetState (every stage state
    // change goes through it), so the per-lookup hold check never walks the stage records.
    u32 pending{}, waiting{};
    void SetState(StageRt& st, St s) {
        if (st.state == St::Pending) {
            --pending;
        } else if (st.state == St::Waiting) {
            --waiting;
        }
        if (s == St::Pending) {
            ++pending;
        } else if (s == St::Waiting) {
            ++waiting;
        }
        st.state = s;
    }
};

namespace {
// sotc: SOTC_PIPE_MANIFEST counters the workers and the 5 s report read.
struct ManifestStats {
    std::atomic<u64> compute_built{};          ///< manifest compute builds done (workers)
    std::atomic<u64> late_graphics{}, late_compute{}; ///< pipelines queued after the boot hold
    std::atomic<u64> quiet_deferred{};         ///< sotc: SOTC_PLAY_QUIET: holds not started because the game drew
    std::atomic<u32> waiting_code{};           ///< stages whose program code has not come
    std::atomic<bool> boot_done{};
};
ManifestStats g_manifest_stats;
} // namespace

// sotc: SOTC CACHE DIAG (vk_pipeline_serialization.cpp): 6 new programs, 7 unused (was: meta on disk; the disk read
// on the command processor was a diagnostic only), 8 new permutations, 9 of them queued to a worker.
constexpr int kCacheDiagCount = 10;
extern std::atomic<u32> g_cache_diag[kCacheDiagCount];
void CacheDiagReport(const char* when);
static void CacheDiagNoteProgram(u64 hash) {
    (void)hash;
    g_cache_diag[6].fetch_add(1, std::memory_order_relaxed);
    if ((g_cache_diag[6].load() + g_cache_diag[8].load()) % 128 == 0) {
        CacheDiagReport("runtime");
    }
}
static void CacheDiagNotePerm(bool async) {
    g_cache_diag[8].fetch_add(1, std::memory_order_relaxed);
    if (async) {
        g_cache_diag[9].fetch_add(1, std::memory_order_relaxed);
    }
    if ((g_cache_diag[6].load() + g_cache_diag[8].load()) % 128 == 0) {
        CacheDiagReport("runtime");
    }
}

// sotc: the saved driver pipeline cache (PipelineCache::SaveDriverCache).
static bool DriverCacheEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_DRIVER_CACHE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

static std::filesystem::path DriverCachePath() {
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::CacheDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "sotc_vk_pipelines.bin";
}
// sotc: SOTC_DRIVER_CACHE_MAX_MB (default 512): size cap of the live driver cache file only (not the pack cache).
// A larger file is moved aside at load (it made the driver keep a multi-GB copy resident); 0 disables the cap.
static size_t DriverCacheMaxBytes() {
    static const size_t cap = [] {
        const char* v = std::getenv("SOTC_DRIVER_CACHE_MAX_MB");
        const unsigned long long mb = (v && v[0]) ? std::strtoull(v, nullptr, 10) : 512ull;
        return static_cast<size_t>(mb) << 20;
    }();
    return cap;
}
// sotc: SOTC_PLAY_QUIET: the pack cache (stage libraries), next to it.
static std::filesystem::path PackCachePath() {
    return DriverCachePath().parent_path() / "sotc_vk_pack.bin";
}

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};
// sotc: SOTC_BIND_DESC_REUSE: the flat buffer bindings are dynamic storage buffers (Pipeline::MarkDynamicFlat), so
// the pools hold that type too.
constexpr static std::array DescriptorHeapSizesDynamic = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBufferDynamic, 2048},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};
static std::span<const vk::DescriptorPoolSize> HeapSizes() {
    if (SotcNonBlocking::BindDescReuse()) {
        return DescriptorHeapSizesDynamic;
    }
    return DescriptorHeapSizes;
}

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

// sotc: the compute part of BuildRuntimeInfo, shared with PrefetchComputeProgram (which builds it
// from a scanned copy of the registers).
static void FillComputeRuntimeInfo(Shader::RuntimeInfo& info, const AmdGpu::ComputeProgram& cs_pgm) {
    info.props.num_user_data = cs_pgm.settings.num_user_regs;
    // sotc: COMPUTE_PGM_RSRC1.VGPRS holds granules - 1, as for the graphics stages (ShaderProgram::NumVgprs). It was
    // num_vgprs * 4, one granule (4 VGPRs) short: 4,222 of the game's 4,260 compute programs use their top granule.
    // Only v_movrel* bounds and the scratch VGPRs read the count (neither occurs in a compute program of the game),
    // but it is part of the shader cache key, so the cached compute shaders are translated once again.
    info.props.num_allocated_vgprs = (cs_pgm.settings.num_vgprs + 1) * 4;
    info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
    info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                 cs_pgm.num_thread_z.full};
    info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                              cs_pgm.IsTgidEnabled(2)};
    info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
}

// sotc: SOTC_VS_DRAW_KIND (info.h). True when a vertex stage of this draw kind uses the shared module: an instanced
// program (VGPR component count 3) on the hardware VS stage, no base vertex SGPR, and the start instance either absent
// (direct draw) or in user SGPR 2 (SotC's indirect draws).
static bool VsDrawKindShared(HwStage stage, u32 num_input_vgprs, u16 vertex_sgpr_offset,
                             u16 instance_sgpr_offset) {
    return Shader::VsDrawKindOn() && stage == HwStage::Vertex && num_input_vgprs >= 3 &&
           vertex_sgpr_offset == 0 &&
           (instance_sgpr_offset == 0 || instance_sgpr_offset == Shader::kVsDrawKindInstanceSgpr);
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(HwStage stage, SwStage l_stage) {
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        // GCN tests depth/stencil before the pixel shader when Z_ORDER asks for it, so fragments
        // that fail never run. A Vulkan shader with storage writes needs EarlyFragmentTests for
        // that; without it the writes of failing fragments land too (SotC deferred decals paint
        // their whole box footprint into the G-buffer: rectangles on the ground). Not with a
        // depth, stencil ref or sample mask export (mrtz_mask bits 0-2), the tests need them.
        // From Pink-shadPS4 (luizgustavs). SotC only (EU and US).
        static const bool early_z_enabled = [] {
            const bool on = Common::ElfInfo::Instance().IsSotc();
            std::fprintf(stderr, "SOTC early fragment tests: %s\n", on ? "on" : "off");
            return on;
        }();
        const auto z_order = regs.depth_shader_control.z_order;
        info.hw.fs.early_fragment_tests =
            early_z_enabled &&
            (z_order == AmdGpu::ZOrder::EarlyZLateZ || z_order == AmdGpu::ZOrder::EarlyZReZ) &&
            (info.hw.fs.mrtz_mask & 0b111) == 0;
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        FillComputeRuntimeInfo(info, liverpool->GetCsRegs());
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = draw_indirect_params.instance_sgpr_offset;
        // sotc: SOTC_VS_DRAW_KIND (info.h): both draw kinds of an instanced program take one key. bind_stage zeroes
        // the SGPR's user data for the indirect kind under the same condition (VsDrawKindIndirect).
        info.sw.vs.instance_sgpr_add = false;
        if (VsDrawKindShared(stage, info.props.num_input_vgprs, info.sw.vs.vertex_sgpr_offset,
                             info.sw.vs.instance_sgpr_offset)) {
            info.sw.vs.instance_sgpr_offset = Shader::kVsDrawKindInstanceSgpr;
            info.sw.vs.instance_sgpr_add = true;
        }
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

// sotc: SOTC_ASYNC_SHADERS=0 builds every graphics pipeline on the command processor, as
// before (draws are never skipped for a pipeline being built).
static bool AsyncShadersEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_SHADERS");
        return !v || v[0] != '0';
    }();
    return enabled;
}

// sotc: SOTC_ASYNC_TRANSLATE=0 translates every graphics shader on the command processor (see
// AsyncPipelineJob::Translate).
static bool AsyncTranslateEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_TRANSLATE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

// sotc: SOTC_ASYNC_COMPUTE=0 builds compute pipelines on the command processor (see
// AsyncPipelineJob).
static bool AsyncComputeEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_COMPUTE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

// sotc: N5 (SOTC_N5_PSO=1, with SOTC_NATIVE). The boot prebuild (WarmUp, WaitPreloadBoot), the
// use counts saved at teardown (SaveUseCounts) and the counters of the compiles that stall the command
// processor or the draw thread (ReportN5Pso).
bool N5PsoEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_N5_PSO");
        return SotcNative::On() && v && v[0] == '1';
    }();
    return enabled;
}

namespace {
using N5Clock = std::chrono::steady_clock;
// Initialized at static initialization: the start of the process, for the grace period.
const N5Clock::time_point g_n5_t0 = N5Clock::now();

enum class N5Kind : u32 { Graphics, Compute, Link, Shader, Count };
constexpr size_t kN5Kinds = static_cast<size_t>(N5Kind::Count);

struct N5Stats {
    std::atomic<u64> boot_total{};    ///< graphics pipelines the boot prebuild queued
    std::atomic<u64> boot_built{};    ///< ... and the workers built
    std::atomic<bool> boot_closed{};  ///< WaitPreloadBoot runs: no more are queued
    std::atomic<s64> boot_start_ns{}; ///< WarmUp start
    std::atomic<s64> boot_end_ns{};   ///< every queued one built (0: not yet)
    std::atomic<bool> warming{};      ///< WarmUp runs
    std::atomic<u64> grace{};         ///< sync compiles inside WarmUp or the grace period
    std::array<std::atomic<u64>, kN5Kinds> kind{}; ///< sync compiles after it, by kind
    std::atomic<u64> ns{};            ///< their time
    std::atomic<u64> max_ns{};        ///< the longest in the window (reset by the report)
};
N5Stats g_n5;
/// Notified, under async_mutex, when a graphics pipeline of the boot prebuild is built.
std::condition_variable_any g_preload_cv;

s64 N5NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(N5Clock::now().time_since_epoch())
        .count();
}

double N5GraceSeconds() {
    static const double seconds = [] {
        const char* v = std::getenv("SOTC_N5_PSO_GRACE_S");
        return v ? std::atof(v) : 30.0;
    }();
    return seconds;
}

// sotc: a pipeline or shader compiled on the command processor (or the draw thread) stalls it. Counted
// per kind after the grace period; the ones inside it are counted apart.
void N5NoteSyncCompile(N5Kind kind, N5Clock::time_point start) {
    if (!N5PsoEnabled()) {
        return;
    }
    if (g_n5.warming.load(std::memory_order_relaxed) ||
        std::chrono::duration<double>(start - g_n5_t0).count() < N5GraceSeconds()) {
        g_n5.grace.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(N5Clock::now() - start).count());
    g_n5.kind[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    g_n5.ns.fetch_add(ns, std::memory_order_relaxed);
    u64 prev = g_n5.max_ns.load(std::memory_order_relaxed);
    while (ns > prev && !g_n5.max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
}

// sotc: N5. A boot prebuild graphics pipeline is built. Called under async_mutex.
void N5NoteBootBuilt() {
    const u64 built = g_n5.boot_built.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_n5.boot_closed.load(std::memory_order_relaxed) &&
        built >= g_n5.boot_total.load(std::memory_order_relaxed)) {
        g_n5.boot_end_ns.store(N5NowNs(), std::memory_order_relaxed);
    }
}

// sotc: N5. Times one compile on the command processor (CompileModule, when no worker pools are given).
struct N5SyncScope {
    N5Kind kind;
    N5Clock::time_point start;
    bool on;
    ~N5SyncScope() {
        if (on) {
            N5NoteSyncCompile(kind, start);
        }
    }
};
} // namespace

void N5SetWarming(bool warming) {
    g_n5.warming.store(warming, std::memory_order_relaxed);
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler, HeapSizes()}, layout_cache{instance} {
    // sotc: SOTC_BIND_DESC_REUSE: persistent sets need set layout handles that live as long as the heap (the layout
    // cache keeps them); without the cache the entries keep rolling-pool sets (checked by the pool serial).
    desc_heap.EnablePersistentSets(SotcNonBlocking::BindDescReuse() && layout_cache.Enabled());
    const auto& vk12_props = instance.GetVk12Properties();
    // NVIDIA's shader compiler (seen on 617.14) hits a breakpoint on any OpFDiv in a shader
    // with RoundingModeRTZ for that width, killing the process. Prefer default rounding there.
    // sotc: test switches for the AMD GCN dust flashes: translate the shaders as for NVIDIA, all at
    // once (SOTC_AMD_AS_NV=1) or one by one (SOTC_NO_RTZ, SOTC_MANUAL_INTERP, SOTC_NO_EXPLICIT_VERTEX,
    // SOTC_CLIP_EMU). Their cache goes to a folder of its own (cache_storage.cpp, ShaderTestTag).
    const auto test_on = [](const char* name) {
        const char* all = std::getenv("SOTC_AMD_AS_NV");
        const char* v = std::getenv(name);
        return (all && all[0] == '1') || (v && v[0] == '1');
    };
    const bool rtz_usable = instance.GetDriverID() != vk::DriverId::eNvidiaProprietary && !test_on("SOTC_NO_RTZ");
    // sotc: the LDS barriers are also needed on AMD running the shaders 64 wide. GCN (RX 470-590,
    // Vega) has no 32-wide waves, so the 32-wide path never engages there, and its compiler may
    // move LDS reads before other lanes' writes just the same: white, flickering characters and
    // smoke, fog colours that changed every run (issues #152, #187, #201). The unofficial
    // SotC-AMD-Fix layer (raulbezerro-dev) fixed it there by reporting the NVIDIA driver, which
    // turns these barriers on. SOTC_AMD64_LDS=0 turns it off.
    const bool amd64_lds = [&] {
        const char* v = std::getenv("SOTC_AMD64_LDS");
        return (!v || std::atoi(v) != 0) && instance.GetVendorID() == 0x1002 &&
               instance.SubgroupSize() >= 64;
    }();
    if (amd64_lds) {
        std::fprintf(stderr, "GPU: LDS barriers on for 64-wide AMD (SOTC_AMD64_LDS=0: off)\n");
    }
    if (instance.IsAmdWave32Forced()) {
        std::fprintf(stderr, "GPU: AMD wave32 on: shaders lowered for 32 lanes, LDS barriers on "
                             "(SOTC_AMD_WAVE32)\n");
    }
    // Wave64 uniform branches on every driver. Tested on a Radeon RX 6650 XT (32-wide path):
    // off, the shadows next to the Wander and Agro on grass get horizontal stripes; on, the
    // image is right and the characters are not black in the shade (save with 5 colossi).
    Shader::Optimization::SetWave64UniformBranchesDefault(true);
    // sotc: SotC interpolates most pixel shader inputs by hand from their three vertex values.
    // Without fragment shader barycentrics (NVIDIA GTX 10xx: no textures, black geometry, issues
    // #57/#68/#102) an auxiliary geometry shader provides them (SOTC_BARYCENTRIC_GS=0 turns it
    // off).
    const auto env_is = [](const char* name, const char* value) {
        const char* v = std::getenv(name);
        return v && std::strcmp(v, value) == 0;
    };
    // sotc: SOTC_NO_BARYCENTRIC stays deleted (N4); the shader test switch SOTC_NO_EXPLICIT_VERTEX comes from upstream.
    const bool khr_barycentric = instance_.IsFragmentShaderBarycentricSupported();
    const bool amd_explicit_vertex =
        instance_.IsAmdShaderExplicitVertexParameterSupported() && !test_on("SOTC_NO_EXPLICIT_VERTEX");
    const bool barycentric_via_gs = !khr_barycentric && !amd_explicit_vertex &&
                                    instance.IsGeometryStageSupported() &&
                                    instance.GetDriverID() == vk::DriverId::eNvidiaProprietary &&
                                    !env_is("SOTC_BARYCENTRIC_GS", "0");
    if (barycentric_via_gs) {
        LOG_WARNING(Render_Vulkan, "No fragment shader barycentrics: emulated by a geometry shader");
    }
    GraphicsPipeline::SerializationSupport::with_gs = barycentric_via_gs;
    // sotc: ARCH-GCN-NATIVE. The page table and the globals buffer are created (or their stages refused) before the
    // profile reads Buf/Tex::ProfileValue: built later, every gcn_tex profile would be refused for a missing globals
    // buffer, and a later refusal would leave pipelines compiled for a stage that is off. Both Inits are idempotent;
    // the rasterizer constructor calls them again (no-op) in its own order.
    SotcArch::AddrTable::Init(instance, scheduler);
    SotcGcn::Globals::Init(instance);
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = rtz_usable && bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = rtz_usable && bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = rtz_usable && bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter = amd_explicit_vertex,
        .supports_fragment_shader_barycentric = khr_barycentric || barycentric_via_gs,
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = khr_barycentric &&
                                      (instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                                       test_on("SOTC_MANUAL_INTERP")),
        // sotc: also whenever the shaders are lowered for fewer than 64 lanes (AMD on the 32-wide
        // path): without the barriers the GPU hung ~2 min into the intro (issue #7).
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp ||
                              instance.SubgroupSize() < 64 || amd64_lds,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                                         test_on("SOTC_CLIP_EMU"),
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .barycentric_via_gs = barycentric_via_gs,
        // sotc: only with a set per stage fitting the device (MaxStageDescriptorSets).
        .stage_bindings = Shader::StageBindingsEnabled() &&
                          instance.GetMaxBoundDescriptorSets() >= Shader::MaxStageDescriptorSets,
        .bda_buffers = BdaTableMode(instance) != 0, // sotc: SOTC_BDA (block table, vk_bda_table.h)
        // sotc: SOTC_N5_BINDLESS, and SOTC_BINDLESS_SAMP: both heaps change the emitted SPIR-V.
        .bindless_images = Shader::BindlessImagesEnabled() || Shader::BindlessSamplersEnabled(),
        // sotc: SOTC_HEAP (the index table per draw, heap_images) and SOTC_SHADER_ADDR (buffer addresses
        // from the V# words; mode 2 = verify, a different profile and so a different cache key). Both off:
        // the profile is the one of today.
        .heap_images = SotcArch::Heap::ProfileValue(),
        .shader_addr = SotcArch::ShaderAddr::Mode(),
        // sotc: ARCH-GCN-NATIVE S2/S3 (gcn_native.h). Both 0 unless their stage is on (profile.h has the rules).
        .gcn_buf = SotcGcn::Buf::ProfileValue(),
        .gcn_tex = SotcGcn::Tex::ProfileValue(),
    };
    // sotc: SOTC_ADDR_FALLBACK. The descriptor-mode profile: what a run without SOTC_SHADER_ADDR builds (gcn_buf is 0
    // there: Buf::ProfileValue needs the table mode). Unused in descriptor mode, where every stage takes `profile`.
    desc_profile = profile;
    desc_profile.shader_addr = 0;
    desc_profile.gcn_buf = 0;
    addr_fallback = profile.shader_addr != 0 && [] {
        const char* v = std::getenv("SOTC_ADDR_FALLBACK");
        return v && v[0] == '1'; // sotc: default off: the 7 Nov cold run broke the image (dark terrain, ghosting)
    }();
    if (addr_fallback) {
        std::fprintf(stderr, "SOTC ADDR FALLBACK: on (table-variant pipelines are built on the workers; their ops take "
                             "the descriptor pipeline meanwhile)\n");
    }
    std::fprintf(stderr,
                 "GPU: shader profile: round to zero %d, AMD explicit vertex %d, barycentrics %d, manual "
                 "interpolation %d, clip distance emulation %d\n",
                 int(profile.support_fp32_round_to_zero), int(profile.supports_amd_shader_explicit_vertex_parameter),
                 int(profile.supports_fragment_shader_barycentric), int(profile.needs_manual_interpolation),
                 int(profile.needs_clip_distance_emulation));
    // sotc: the cache is read by the Presenter once it can draw (Presenter::Presenter), with
    // the loading screen on meanwhile.

    // sotc: the VkPipelineCache starts from the copy saved on disk (SaveDriverCache). Without it
    // every start after an update of the port recompiled all ~3000 pipelines in the driver: about
    // 2 minutes of loading screen, since the driver's own cache is tied to the executable. The
    // driver checks the header (vendor, device, driver version) and ignores a stale copy.
    // SOTC_DRIVER_CACHE=0 turns it off.
    // sotc: one saved cache file -> its bytes (empty: none, truncated, another GPU or driver).
    // sotc: SOTC_CACHE_MAP: the file is mapped (read in place) instead of copied into a vector; see CacheFileBytes.
    const auto load_cache = [&](CacheFileBytes& out, const std::filesystem::path& path, const char* what,
                                size_t max_bytes = 0) {
        std::vector<u8>& data = out.data;
        // sotc: SOTC_DRIVER_CACHE_MAX_MB: an oversize live cache is renamed to <name>.oversize (one backup, an older
        // one is overwritten) and not loaded; the caller then creates an empty cache.
        if (max_bytes) {
            std::error_code cap_ec;
            const auto file_bytes = std::filesystem::file_size(path, cap_ec);
            if (!cap_ec && file_bytes > max_bytes) {
                auto backup = path;
                backup += L".oversize";
                std::filesystem::remove(backup, cap_ec);
                std::filesystem::rename(path, backup, cap_ec);
                std::fprintf(stderr,
                             "SOTC driver cache: %llu MB exceeds SOTC_DRIVER_CACHE_MAX_MB=%zu, starting a fresh cache "
                             "(old file kept as .oversize)\n",
                             static_cast<unsigned long long>(file_bytes >> 20), max_bytes >> 20);
                return;
            }
        }
        if (std::FILE* f = _wfopen(path.c_str(), L"rb")) {
            // sotc: ftell's long is only 32 bits on Windows; a GPL pack can exceed 2 GiB.
            // Reject incompatible headers before allocating/reading the whole cache.
            std::error_code size_ec;
            const auto bytes = std::filesystem::file_size(path, size_ec);
            std::array<u8, 32> header{};
            const size_t header_read = std::fread(header.data(), 1, header.size(), f);
            const char* mismatch = SotcCache::HeaderMismatch(std::span{header}.first(header_read),
                                                             instance.GetVendorID(), instance.GetDeviceID(),
                                                             instance.GetPipelineCacheUUID());
            if (mismatch) {
                std::fprintf(stderr, "SOTC %s: %s; starting empty\n", what, mismatch);
            } else if (!size_ec && bytes >= header.size() && SotcNonBlocking::CacheMap() &&
                       out.Map(path, static_cast<size_t>(bytes))) {
                // sotc: SOTC_CACHE_MAP: read in place (the vector read below is the fallback)
            } else if (!size_ec && bytes >= header.size() && bytes <= data.max_size()) {
                data.resize(static_cast<size_t>(bytes));
                std::copy(header.begin(), header.end(), data.begin());
                const size_t remaining = data.size() - header.size();
                if (std::fread(data.data() + header.size(), 1, remaining, f) != remaining) {
                    data.clear();
                }
            }
            std::fclose(f);
        }
    };
    // sotc: the VkPipelineCache from those bytes (empty on rejection; data cleared then).
    const auto create_cache = [&](CacheFileBytes& data, const char* what) {
        vk::PipelineCacheCreateInfo cache_ci{
            .initialDataSize = data.Size(),
            .pInitialData = data.Empty() ? nullptr : data.Data(),
        };
        auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(cache_ci);
        if (cache_result != vk::Result::eSuccess && !data.Empty()) {
            std::fprintf(stderr, "SOTC %s: saved copy rejected (%s), starting empty\n", what,
                         vk::to_string(cache_result).c_str());
            data.Release();
            auto retry = instance.GetDevice().createPipelineCacheUnique({});
            cache_result = retry.result;
            cache = std::move(retry.value);
        }
        ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
                   vk::to_string(cache_result));
        if (!data.Empty()) {
            std::fprintf(stderr, "SOTC %s: %zu MB loaded%s\n", what, data.Size() >> 20, // sotc: MB
                         data.data.empty() ? " (file mapped)" : "");
        }
        return std::move(cache);
    };
    // sotc: SOTC_CACHE_MAP: each file's bytes are dropped right after its cache object exists (size and hash taken
    // first), so neither file stays resident beside the driver's copy. 0: both buffers live to the end, as before.
    const bool release_early = SotcNonBlocking::CacheMap();
    const bool split_cache = DriverCacheEnabled() && SotcNonBlocking::PlayQuiet();
    CacheFileBytes driver_cache_data;
    if (DriverCacheEnabled()) {
        load_cache(driver_cache_data, DriverCachePath(), "driver cache", DriverCacheMaxBytes());
    }
    pipeline_cache = create_cache(driver_cache_data, "driver cache");
    saved_driver_cache_size = driver_cache_data.Size();
    bool source_loaded = false; // sotc: the pack list's source: the pack cache when split, else the driver cache
    u64 source_hash = 0;
    if (!split_cache) {
        source_loaded = !driver_cache_data.Empty();
        source_hash = source_loaded ? XXH3_64bits(driver_cache_data.Data(), driver_cache_data.Size()) : 0;
    }
    if (release_early) {
        driver_cache_data.Release();
    }
    // sotc: SOTC_PLAY_QUIET: the stage libraries get their own cache object and file (sotc_vk_pack.bin): the pack
    // publication writes it once per pack compile (and the exit, when it grew), so the periodic saver's
    // vkGetPipelineCacheData reads only the live cache. Libraries are VkPipeline objects: a link made from them with
    // the live cache does not care which cache created them. (A merge into one cache at startup would keep the
    // periodic save as large as before; the split keeps it small.) The pack library list is bound to this file.
    CacheFileBytes pack_data;
    if (split_cache) {
        load_cache(pack_data, PackCachePath(), "pack cache");
        pack_cache = create_cache(pack_data, "pack cache");
        saved_pack_cache_size = pack_data.Size();
        GplLibraryCache::SetLibraryCache(*pack_cache);
        source_loaded = !pack_data.Empty();
        source_hash = source_loaded ? XXH3_64bits(pack_data.Data(), pack_data.Size()) : 0;
        if (release_early) {
            pack_data.Release();
        }
    }
    driver_cache_loaded = source_loaded; // sotc: compatible header; feedback still proves individual hits
    loaded_driver_cache_hash = source_hash;
    if (DriverCacheEnabled()) {
        // New pipelines (first visits) are saved every minute while playing: the game is often
        // closed by killing the process, which skips the destructor.
        driver_cache_saver = std::jthread([this](std::stop_token stop) {
            using Clock = std::chrono::steady_clock;
            auto last_save = Clock::now();
            const bool quiet = SotcNonBlocking::PlayQuiet();
            while (!stop.stop_requested()) {
                bool requested = false;
                {
                    // sotc: every 60 s, or now when RequestDriverCacheSave asks (SOTC_NB_IO). SOTC_PLAY_QUIET: polled
                    // every 5 s; the periodic save (60 s apart) runs only at a loading screen or after 5 s without a
                    // pipeline creation, so vkGetPipelineCacheData never runs under a draw's link.
                    std::unique_lock lk{driver_cache_cv_mutex};
                    driver_cache_cv.wait_for(lk, stop, std::chrono::seconds{quiet ? 5 : 60},
                                             [this] { return driver_cache_save_requested; });
                    requested = driver_cache_save_requested;
                    driver_cache_save_requested = false;
                }
                if (stop.stop_requested()) {
                    break;
                }
                if (pack_publish_state.load(std::memory_order_acquire) == 1) {
                    RunPackPublish(); // sotc: SOTC_GPL_PACK_LEAN: the pack cache, then the list
                }
                const auto now = Clock::now();
                bool save = requested;
                if (!save && now - last_save >= std::chrono::seconds{60}) {
                    const u64 now_ns = SotcJobClockNs();
                    const u64 idle_ns = now_ns - std::min(now_ns, SotcPipeFeedback::LastCreateNs());
                    save = !quiet || BbOverlay::LoadingShown() || idle_ns > 5'000'000'000ull;
                }
                if (save) {
                    last_save = now;
                    SaveDriverCache();
                }
            }
        });
    }

    // sotc: pipeline workers for SOTC_ASYNC_SHADERS (see AsyncPipelineJob).
    if (AsyncShadersEnabled()) {
        // The hardware threads less four, 2 to 12. They are busy only on a first visit, while the
        // game's own threads mostly wait for them (cold caches, attract demo, 16 threads: worst
        // frame 4.8 s with 6 workers, 3.4 s with 12; one worker, the old default on 4 threads:
        // 18 s).
        // sotc: with SOTC_CPUSETS=1 the default is capped at 4 (see BbThreads::PlaceThread).
        const int threads = static_cast<int>(std::thread::hardware_concurrency());
        const int default_cap = BbThreads::CpuSetsEnabled() ? 4 : 12;
        const unsigned count = static_cast<unsigned>(std::clamp(threads - 4, 2, default_cap));
        // sotc: SOTC_GPL_URGENT_WORKERS: the first n workers serve only the Urgent and Predict lanes (a draw's missing
        // libraries, the libraries of the materials just created), so those builds never queue behind translations,
        // compute prebuilds or optimized pipeline builds. At least two general workers stay.
        const unsigned urgent = SotcNonBlocking::GplUrgentWorkers();
        reserved_workers = count >= urgent + 2 ? urgent : 0;
        for (unsigned i = 0; i < count; ++i) {
            const bool urgent_only = i < reserved_workers;
            async_workers.emplace_back(
                [this, urgent_only](std::stop_token token) { AsyncPipelineWorker(token, urgent_only); });
        }
    }
    // sotc: SOTC_COMPUTE_FAST: the recorder's fast builds queue their optimized build on the workers.
    if (SotcNonBlocking::ComputeFast() && !async_workers.empty()) {
        ComputePipeline::QueueOptimizedHook = [this](ComputePipeline* pipeline) { QueueComputeOptimized(pipeline); };
        std::fprintf(stderr, "SOTC COMPUTE FAST: on (driver-cache probe of the optimized build %s)\n",
                     SotcPipeFeedback::g_compile_control.load(std::memory_order_relaxed) ? "on" : "off");
    }
    // sotc: SOTC_ASYNC_COMPUTE_XLATE=1 (off by default), see ComputePrefetchEnabled.
    const uint64_t private_after_cache_load = SotcPipeManifest::ProcessPrivateBytes();
    LogRam("after cache load"); // sotc: SOTC RAM
    compute_prefetch = !async_workers.empty() && [] {
        const char* v = std::getenv("SOTC_ASYNC_COMPUTE_XLATE");
        return v && v[0] == '1';
    }();
    // sotc: the environment snapshot cannot resolve device-dependent gates; report them once here.
    std::fprintf(stderr,
                 "SOTC PIPE CONFIG: driver-cache %d loaded %d; workers %zu (urgent-only %u); nodrop %d requested %d; "
                 "shader-only %d; pack %d; predict %d; precompile %d; compute-prefetch %d; private %llu MB after cache load\n",
                 DriverCacheEnabled(), driver_cache_loaded, async_workers.size(), reserved_workers,
                 SotcNonBlocking::GplNoDrop(),
                 SotcNonBlocking::GplNoDropRequested(), SotcNonBlocking::GplShaderKeys(),
                 SotcNonBlocking::GplPackLibs(), SotcNonBlocking::GplPredict(), SotcPretranslate::Precompile(),
                 compute_prefetch, static_cast<unsigned long long>(private_after_cache_load >> 20));
}

PipelineCache::~PipelineCache() {
    ComputePipeline::QueueOptimizedHook = nullptr; // sotc: SOTC_COMPUTE_FAST (the workers stop with this object)
    SaveUseCounts(); // sotc: N5 (no-op without SOTC_N5_PSO)
    if (driver_cache_saver.joinable()) {
        driver_cache_saver.request_stop();
        driver_cache_saver.join();
        SaveDriverCache(true);
        FinalPackPublish(); // sotc: SOTC_PLAY_QUIET: the pack cache, when it grew since its publication
    }
}

void PipelineCache::RequestDriverCacheSave() {
    if (!driver_cache_saver.joinable()) {
        SaveDriverCache();
        return;
    }
    {
        std::scoped_lock lk{driver_cache_cv_mutex};
        driver_cache_save_requested = true;
    }
    driver_cache_cv.notify_all();
}

bool PipelineCache::SaveDriverCache(bool final_save, size_t* written_size, u64* written_hash) {
    if (!DriverCacheEnabled() || !pipeline_cache) {
        return false;
    }
    return SaveCacheFile(*pipeline_cache, DriverCachePath(), saved_driver_cache_size, final_save, written_size,
                         written_hash, "driver cache");
}

bool PipelineCache::SavePackCache(size_t* written_size, u64* written_hash) {
    if (!DriverCacheEnabled() || !pack_cache) {
        return false;
    }
    return SaveCacheFile(*pack_cache, PackCachePath(), saved_pack_cache_size, true, written_size, written_hash,
                         "pack cache");
}

bool PipelineCache::SaveCacheFile(vk::PipelineCache cache, const std::filesystem::path& path, size_t& saved_size,
                                  bool final_save, size_t* written_size, u64* written_hash, const char* what) {
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::DriverCacheSave};
    std::scoped_lock lk{driver_cache_save_mutex};
    size_t size = 0;
    const vk::Device device = instance.GetDevice();
    std::optional<SotcBlockLedger::Scope> get_timer{std::in_place, SotcBlockLedger::DriverCacheGet};
    if (device.getPipelineCacheData(cache, &size, nullptr) != vk::Result::eSuccess ||
        size == 0 || (written_size == nullptr && size == saved_size)) {
        return false;
    }
    // sotc: P5 of the non-blocking frame. The file is rewritten whole (1.9 GB, 25 s of disk writes on the saver thread
    // in a warm session), so the periodic saves wait until it grew by 5% or 64 MiB; the save at shutdown writes any
    // growth. SOTC_DRIVER_CACHE_ALWAYS=1 keeps the old rule (any growth).
    static const bool always = [] {
        const char* v = std::getenv("SOTC_DRIVER_CACHE_ALWAYS");
        return v && v[0] == '1';
    }();
    if (!final_save && !always && saved_size != 0 && size > saved_size &&
        size - saved_size < std::max<size_t>(size_t{64} << 20, saved_size / 20)) {
        return false;
    }
    // sotc: SOTC_DRIVER_CACHE_MAX_MB: still saved, but the next launch will move it aside (warn once).
    if (path == DriverCachePath() && DriverCacheMaxBytes() && size > DriverCacheMaxBytes()) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr,
                         "SOTC driver cache: %zu MB exceeds SOTC_DRIVER_CACHE_MAX_MB=%zu; saved anyway, the next "
                         "launch will start fresh\n",
                         size >> 20, DriverCacheMaxBytes() >> 20);
        }
    }
    // Written next to it and renamed: a crash while writing keeps the old copy.
    auto tmp = path;
    tmp += L".tmp";
#ifdef _WIN32
    // sotc: SOTC_CACHE_MAP: the driver writes the blob straight into a mapping of the .tmp file (file-backed pages, no
    // private copy of the cache's size); the file is cut to the size written, then renamed as below. A mapping failure
    // falls back to the vector path.
    if (SotcNonBlocking::CacheMap()) {
        HANDLE file = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE mapping = file == INVALID_HANDLE_VALUE
                             ? nullptr
                             : CreateFileMappingW(file, nullptr, PAGE_READWRITE, static_cast<DWORD>(u64{size} >> 32),
                                                  static_cast<DWORD>(size & 0xffffffffu), nullptr);
        void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, size) : nullptr;
        if (view) {
            size_t got = size;
            bool ok = device.getPipelineCacheData(cache, &got, view) == vk::Result::eSuccess && got <= size;
            get_timer.reset();
            const u64 hash = ok && written_hash ? XXH3_64bits(view, got) : 0;
            ok = ok && FlushViewOfFile(view, 0);
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            if (ok && got < size) {
                LARGE_INTEGER end{};
                end.QuadPart = static_cast<LONGLONG>(got);
                ok = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
            }
            ok = CloseHandle(file) && ok;
            std::error_code ec;
            if (ok) {
                std::filesystem::rename(tmp, path, ec);
            }
            if (!ok || ec) {
                std::filesystem::remove(tmp, ec);
                return false;
            }
            saved_size = got;
            if (written_size) {
                *written_size = got;
            }
            if (written_hash) {
                *written_hash = hash;
            }
            std::fprintf(stderr, "SOTC %s: %zu KB saved (file mapped)\n", what, got >> 10);
            return true;
        }
        if (mapping) {
            CloseHandle(mapping);
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
    }
#endif
    std::vector<u8> data(size);
    if (device.getPipelineCacheData(cache, &size, data.data()) != vk::Result::eSuccess) {
        return false;
    }
    get_timer.reset();
    data.resize(size);
    std::FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        return false;
    }
    const bool wrote = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool closed = std::fclose(f) == 0;
    const bool ok = wrote && closed; // sotc: buffered writes can fail only at close
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
    }
    if (ok && !ec) {
        saved_size = size;
        if (written_size) {
            *written_size = size;
        }
        if (written_hash) {
            *written_hash = XXH3_64bits(data.data(), data.size());
        }
        std::fprintf(stderr, "SOTC %s: %zu KB saved\n", what, size >> 10);
        return true;
    } else {
        std::filesystem::remove(tmp, ec);
    }
    return false;
}

// sotc: SOTC_TINY_ASYNC (see GetGraphicsPipeline). 0 (unset): a tiny draw (<= 6 indices) whose
// pipeline is missing is built on the command processor, as before. 1: it is queued at the front
// of the async queue and the draw is skipped until the pipeline is ready, as for geometry draws.
// 2: nothing changes, the build stays inline, and the draws that =1 would defer are counted and
// logged (first 20).
//
// Why tiny draws were built inline: a skipped full-screen pass leaves its target stale, or
// undefined (black) the first time. Geometry draws accept that for the build time. Mode 1 keeps
// the inline build for draws that write a display buffer: a stale or black presented frame is
// seen directly. Other tiny draws (post-processing into intermediates, HUD quads, depth writes)
// are deferred: their result is one or more frames old until the pipeline is ready. Their shaders
// are still translated on the command processor (async_translate_ok excludes tiny draws).
// Known risk of mode 1 (review, 29 Oct): a full-screen pass the game runs only once (a lookup
// table or noise texture made at boot or area load) is skipped on its first and only run, so its
// target stays wrong for good. Mode 1 stays experimental for that reason; the per-area prewarm
// (SOTC_AREA_PREWARM) is the intended fix for first-use builds, since it compiles the pipeline
// before the draw instead of skipping the draw.
static int TinyAsyncMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_TINY_ASYNC");
        if (!v) {
            return 0;
        }
        return v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

namespace {
// sotc: tiny jobs queued (SOTC_TINY_ASYNC=1). They form a run right after the compute and
// translation jobs, before the priority jobs. Guarded by PipelineCache::async_mutex. Only one
// PipelineCache exists per process; the workers reset it when they drain the queue.


// sotc: display buffers as guest ranges [begin, end), from SotcTinyAsyncNoteDisplayBuffer.
std::mutex g_display_mutex;
std::vector<std::pair<u64, u64>> g_display_ranges;

// sotc: counters of SOTC_TINY_ASYNC, printed by SotcTinyAsyncReport.
struct TinyAsyncStats {
    std::atomic<u64> queued{};             ///< =1: new tiny pipelines queued at the front
    std::atomic<u64> moved{};              ///< =1: queued pipelines moved to the front
    std::atomic<u64> skipped{};            ///< =1: tiny draws skipped while their pipeline builds
    std::atomic<u64> display_inline{};     ///< =1: built inline, a color target is a display buffer
    std::atomic<u64> would_defer{};        ///< =2: draws that =1 would defer
    std::atomic<u64> would_defer_display{}; ///< =2: of those, the ones on a display buffer
    std::atomic<u64> sync_builds{};        ///< tiny draws built on the command processor
    std::atomic<u64> sync_ns{};            ///< time of those builds
};
TinyAsyncStats g_tiny_stats;
std::atomic<u32> g_tiny_logged{};

// sotc: SOTC_TINY_GPL=1 (off by default; needs SOTC_PIPE_GPL=1 too, which enables the extension). A tiny
// draw (<= 6 indices) whose pipeline is missing or still queued on a worker is served at once by a
// fast-linked pipeline: its four libraries are created on the command processor (the stage modules are
// translated there already, the draw's shaders are not skipped), then linked without optimization. The
// optimized build is queued on a worker and replaces the linked pipeline when collected, as the GPL
// path does for geometry draws. Only with async workers and an eligible pipeline; otherwise, or when the
// fast link is refused, the full build runs on the command processor as before. Takes precedence over
// SOTC_TINY_ASYNC when both are set. Any other value (or unset): nothing changes.
bool TinyGplOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_TINY_GPL");
        return v && v[0] == '1';
    }();
    return on;
}

// sotc: SOTC_TINY_GPL counters, printed by ReportPipeGpl (the 5 s report, with SOTC_PIPE_GPL=1).
struct TinyGplStats {
    std::atomic<u64> linked{};   ///< tiny draws served by a fast-linked pipeline
    std::atomic<u64> queued{};   ///< optimized builds queued behind them
    std::atomic<u64> lib_ns{};   ///< command processor time of those draws, minus the link time
    std::atomic<u64> link_ns{};  ///< time of their links (vkCreateGraphicsPipelines with the libraries)
};
TinyGplStats g_tiny_gpl;

// sotc: SOTC_GPL_NODROP counters (command processor), printed by ReportPipeGpl.
struct NoDropStats {
    std::atomic<u64> links{};          ///< pipelines fast-linked for a draw that found none built
    std::atomic<u64> sync_lib_draws{}; ///< of those, links that built libraries on the command processor
    std::atomic<u64> sync_libs{};      ///< libraries built on the command processor
    std::atomic<u64> sync_lib_ns{};    ///< their time (the link time excluded)
    std::atomic<u64> link_refused{};   ///< linkable draws whose libraries or link the driver refused
    std::atomic<u64> sync_full{};      ///< full pipelines built on the command processor (never a drop)
    std::atomic<u64> sync_full_ns{};
    std::atomic<u64> sync_full_unlinkable{}; ///< of those: the fast link does not cover the state
    std::atomic<u64> sync_xlate{};     ///< lookups whose pending stage translations were made on the command processor
    std::atomic<u64> sync_xlate_ns{};
    std::atomic<u64> drop_translate{}; ///< drops: a stage translation still pending (table mode, or refused)
    std::atomic<u64> boot_libs_queued{}, boot_libs_done{}; ///< manifest library jobs
    // sotc: SOTC_GPL_PREDICT (PredictLibraries).
    std::atomic<u64> pred_slots{};        ///< material pass slots read (new VS+PS pairs)
    std::atomic<u64> pred_jobs{};         ///< predicted library jobs queued
    std::atomic<u64> pred_no_template{};  ///< slot checks put back: no pass state known yet
    std::atomic<u64> pred_no_program{};   ///< slot checks put back: a program not in the program cache yet
    std::atomic<u64> pred_pack_loads{};   ///< pack loads queued for a predicted program
    std::atomic<u64> pred_promoted{};     ///< pack translations of a predicted program moved to the front
    std::atomic<u64> pred_dropped{};      ///< slots given up (programs never came, or the state cannot be linked)
    std::atomic<u64> pred_waiting{};      ///< slots waiting now (a gauge)
    std::atomic<u64> pre_deferred{};      ///< pack translation feeds held back while library jobs run
    // sotc: SOTC_GPL_PARALLEL_LIBS (BuildLibrariesParallel).
    std::atomic<u64> par_waits{};         ///< draws that waited for their two library jobs
    std::atomic<u64> par_wait_ns{};       ///< their time on the command processor (jobs it took included)
    std::atomic<u64> par_taken{};         ///< of those jobs, the ones the command processor ran itself
};
NoDropStats g_nodrop;

// sotc: SOTC_PIPE_GPL. A fast-linked pipeline replaced by its optimized build is kept alive until the
// process ends, never destroyed. A BackOp holds a raw pointer to the pipeline it was captured with
// (Liverpool::CaptureBackOp) and the back thread records it later, so a delete scheduled for the
// front thread's tick could free a pipeline that a queued draw still binds (device lost). Each
// retired pipeline is one VkPipeline; the count is the "optimized replacements" of the GPL report.
std::mutex g_retired_mutex;
std::vector<std::unique_ptr<GraphicsPipeline>>& RetiredLinkedPipelines() {
    static auto* retired = new std::vector<std::unique_ptr<GraphicsPipeline>>; // never freed
    return *retired;
}
void RetireLinkedPipeline(std::unique_ptr<GraphicsPipeline> linked) {
    std::scoped_lock lock{g_retired_mutex};
    RetiredLinkedPipelines().push_back(std::move(linked));
}
} // namespace

// sotc: SOTC_TINY_ASYNC=1 treats a color target that lies in a display buffer as a presentation
// pass. Called for each display buffer VideoOut registers (address, size in bytes). While no
// display buffer is known, every draw counts as one (fail-safe: mode 1 then builds inline).
void SotcTinyAsyncNoteDisplayBuffer(u64 address, u64 size) {
    if (address == 0 || size == 0) {
        return;
    }
    std::scoped_lock lock{g_display_mutex};
    // sotc: the game registers the same buffers again (mode and area changes): keep each base once,
    // so the cap counts distinct buffers.
    for (auto& range : g_display_ranges) {
        if (range.first == address) {
            range.second = address + size;
            return;
        }
    }
    if (g_display_ranges.size() < 64) {
        g_display_ranges.emplace_back(address, address + size);
    }
}

// sotc: writes a report line for the 5 s report: counts since the previous call, and the rate of
// skipped draws over `seconds`. Call from one thread only.
void SotcTinyAsyncReport(std::string& out, double seconds) {
    static std::array<u64, 8> prev{};
    const std::array<u64, 8> now{
        g_tiny_stats.queued.load(std::memory_order_relaxed),
        g_tiny_stats.moved.load(std::memory_order_relaxed),
        g_tiny_stats.skipped.load(std::memory_order_relaxed),
        g_tiny_stats.display_inline.load(std::memory_order_relaxed),
        g_tiny_stats.would_defer.load(std::memory_order_relaxed),
        g_tiny_stats.would_defer_display.load(std::memory_order_relaxed),
        g_tiny_stats.sync_builds.load(std::memory_order_relaxed),
        g_tiny_stats.sync_ns.load(std::memory_order_relaxed),
    };
    std::array<u64, 8> d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = now[i] - prev[i];
    }
    prev = now;
    // sotc: no line when nothing happened in the window (mode 0 has only sync-build counts).
    bool any = false;
    for (const u64 v : d) {
        any |= v != 0;
    }
    if (!any) {
        return;
    }
    const double skip_rate = seconds > 0.0 ? static_cast<double>(d[2]) / seconds : 0.0;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "SOTC tiny async: mode %d, queued %llu, moved %llu, skipped %llu (%.1f/s), "
                  "display inline %llu, would defer %llu (display kept inline %llu), sync tiny "
                  "builds %llu (%.1f ms)\n",
                  TinyAsyncMode(), static_cast<unsigned long long>(d[0]),
                  static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[2]),
                  skip_rate, static_cast<unsigned long long>(d[3]),
                  static_cast<unsigned long long>(d[4]), static_cast<unsigned long long>(d[5]),
                  static_cast<unsigned long long>(d[6]), static_cast<double>(d[7]) / 1e6);
    out += line;
}

// sotc: a pipeline built on the command processor that took longer than 30 ms is logged with
// the reason it was not built on a pipeline worker (SOTC_SYNC_PIPELINE_MS changes the limit).
static void LogSyncPipeline(const char* kind, const char* reason,
                            std::chrono::steady_clock::time_point start, u64 hash_a, u64 hash_b) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    static const double min_ms = [] {
        const char* v = std::getenv("SOTC_SYNC_PIPELINE_MS");
        return v ? std::atof(v) : 30.0;
    }();
    static const auto t0 = start;
    static std::atomic<int> logged{0};
    if (ms < min_ms || logged.fetch_add(1, std::memory_order_relaxed) >= 2000) {
        return;
    }
    const double at = std::chrono::duration<double, std::milli>(start - t0).count();
    std::fprintf(stderr, "SOTC SYNC PIPELINE %s (%s) %.0f ms, shaders %016llx %016llx, at %.1f\n",
                 kind, reason, ms, static_cast<unsigned long long>(hash_a),
                 static_cast<unsigned long long>(hash_b), at);
}

namespace {
// sotc: SOTC_ADDR_FALLBACK counters, printed by ReportAddrFallback.
struct AddrFallbackStats {
    std::atomic<bool> on{};
    std::atomic<u64> draws{};      ///< draws recorded with a descriptor-mode pipeline
    std::atomic<u64> dispatches{}; ///< dispatches recorded with a descriptor-mode pipeline
    std::atomic<s64> pending{};    ///< table pipelines queued and not built yet
    std::atomic<u64> ready{};      ///< table pipelines built (installed for the next op of their key)
};
AddrFallbackStats g_addr_fb;
} // namespace

// sotc: a pipeline built on a pipeline worker that took SOTC_SLOW_BUILD_MS (default 500) or longer
// is logged: the recorder waits for compute pipelines at their dispatch, so on drivers that compile
// slowly (Intel Arc users saw 13 s freezes on first visits) these are the freezes.
void LogSlowBuild(const char* kind, std::chrono::steady_clock::time_point start) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    static const double min_ms = [] {
        const char* v = std::getenv("SOTC_SLOW_BUILD_MS");
        return v ? std::atof(v) : 500.0;
    }();
    static std::atomic<int> logged{0};
    if (ms >= min_ms && logged.fetch_add(1, std::memory_order_relaxed) < 500) {
        std::fprintf(stderr, "SOTC SLOW BUILD %s %.0f ms\n", kind, ms);
    }
}

bool PipelineCache::AsyncPipelineEligible() const {
    return AsyncIneligibleReason() == nullptr;
}

// sotc: why a pipeline cannot be built on a worker (nullptr: it can). The reason goes to the
// SOTC SYNC PIPELINE log, to tell which blocker costs the most stalls.
const char* PipelineCache::AsyncIneligibleReason() const {
    // Off the command processor the guest registers belong to later draws: only pipelines whose
    // construction reads nothing from them. Vertex inputs are dynamic state (otherwise they are
    // read from the vertex buffer sharps), and no image needs its sharp for its binding count.
    if (!instance.IsVertexInputDynamicState()) {
        return "no dynamic vertex input";
    }
    if (EmulatorSettings.IsShaderCollect()) {
        return "shader collect";
    }
    for (const auto* info : infos) {
        if (!info) {
            continue;
        }
        for (const auto& image : info->images) {
            if (image.array_size == 0 &&
                image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                return "dynamic mip index";
            }
        }
    }
    return nullptr;
}

// sotc: SOTC_ASYNC_PRELOAD=0 builds the cached pipelines at boot on the command processor, one
// after the other, as before. With an empty driver cache (a driver update) that took ~100 s of
// black window; the workers now build them while the game starts, and a draw whose pipeline is
// not built yet is skipped (a compute bind waits for it, as for any deferred compute pipeline).
bool PipelineCache::AsyncPreloadEnabled() const {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_PRELOAD");
        return !v || v[0] != '0';
    }();
    return enabled && !async_workers.empty() && !EmulatorSettings.IsShaderCollect();
}

// sotc: SOTC_PIPE_PREDICT (gpu/sotc/pipe_predict.h). Command processor only.
struct PipePredictState {
    /// The rest of a pass's key (stage hashes zeroed) and its pixel layout, from the first draw of a known slot.
    struct Template {
        bool valid{};
        GraphicsPipelineKey key{};
        std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers> color_buffers{};
        u32 mrtz_mask{};
        u32 num_samples{};
    };
    std::array<Template, 16> templates{};
    /// (VS hash, PS hash) of every material slot read -> its pass slot k.
    std::unordered_map<u64, u8> pair_k;
    std::array<std::vector<SotcPipePredict::Slot>, 16> blocked; ///< slots waiting for their pass template
    std::deque<SotcPipePredict::Slot> ready;                    ///< template known, not built yet
    std::vector<SotcPipePredict::Slot> no_program;              ///< programs not translated yet: tried again later
    std::unordered_set<GraphicsPipelineKey> predicted;          ///< queued keys no draw has found yet
    std::unordered_set<GraphicsPipelineKey> inflight;           ///< queued keys not collected yet
    std::vector<SotcPipePredict::Slot> scratch;
    u64 calls{};
    // Counters (report line).
    u64 slots_new{}, slots_dup{}, queued{}, existing{}, no_template_now{}, no_vs{}, no_ps{}, no_layout{},
        ineligible{}, hits{}, draw_new_known{}, template_conflicts{}, templates_learned{}, lazy_loaded{},
        cp_ns{}, hits_built{}, unstable_skipped{}, busy_waits{};
    /// Template conflicts by key field (kPredictFields order), and the conflicts of each pass slot.
    std::array<u64, 12> conflict_fields{};
    std::array<u64, 16> conflict_k{};
    int conflict_lines{};
    std::chrono::steady_clock::time_point last_report{std::chrono::steady_clock::now()};
};

static u64 PredictPairKey(u64 vs, u64 ps) {
    return HashCombine(vs, ps);
}

// The key fields after the stage hashes, by offset (GraphicsPipelineKey, vk_graphics_pipeline.h).
struct PredictField {
    const char* name;
    size_t begin, end;
};
static const std::array<PredictField, 12> kPredictFields{{
    {"vertex formats", offsetof(GraphicsPipelineKey, vertex_buffer_formats),
     offsetof(GraphicsPipelineKey, patch_control_points)},
    {"patch", offsetof(GraphicsPipelineKey, patch_control_points), offsetof(GraphicsPipelineKey, num_color_attachments)},
    {"color count", offsetof(GraphicsPipelineKey, num_color_attachments), offsetof(GraphicsPipelineKey, color_buffers)},
    {"color formats", offsetof(GraphicsPipelineKey, color_buffers), offsetof(GraphicsPipelineKey, blend_controls)},
    {"blend", offsetof(GraphicsPipelineKey, blend_controls), offsetof(GraphicsPipelineKey, write_masks)},
    {"write masks", offsetof(GraphicsPipelineKey, write_masks), offsetof(GraphicsPipelineKey, cb_shader_mask)},
    {"shader mask", offsetof(GraphicsPipelineKey, cb_shader_mask), offsetof(GraphicsPipelineKey, logic_op)},
    {"logic op", offsetof(GraphicsPipelineKey, logic_op), offsetof(GraphicsPipelineKey, num_samples)},
    {"samples", offsetof(GraphicsPipelineKey, num_samples), offsetof(GraphicsPipelineKey, mrt_mask)},
    {"mrt mask", offsetof(GraphicsPipelineKey, mrt_mask), offsetof(GraphicsPipelineKey, mrt_mask) + sizeof(u32)},
    {"depth/prim", offsetof(GraphicsPipelineKey, mrt_mask) + sizeof(u32), sizeof(GraphicsPipelineKey)},
    {"(none)", 0, 0},
}};

void PipelineCache::PredictPipelines() {
    if (!pipe_predict) {
        pipe_predict = std::make_shared<PipePredictState>();
    }
    auto& st = *pipe_predict;
    ++st.calls;
    const auto t0 = std::chrono::steady_clock::now();
    // New materials (one per draw: TakeSlots reads guest memory).
    st.scratch.clear();
    SotcPipePredict::TakeSlots(st.scratch, 1);
    if (SotcNonBlocking::GplPredict()) {
        PredictLibraries(&st.scratch); // sotc: SOTC_GPL_PREDICT, the same slots
    }
    for (const auto& s : st.scratch) {
        const auto [it, fresh] = st.pair_k.try_emplace(PredictPairKey(s.vs, s.ps), s.k);
        if (!fresh) {
            ++st.slots_dup;
            continue;
        }
        ++st.slots_new;
        if (s.k < st.templates.size() && st.templates[s.k].valid) {
            st.ready.push_back(s);
        } else if (s.k < st.blocked.size()) {
            ++st.no_template_now;
            st.blocked[s.k].push_back(s);
        }
    }
    // Slots whose programs were not translated yet, again every 4096 draws.
    if ((st.calls & 4095) == 0 && !st.no_program.empty()) {
        st.ready.insert(st.ready.end(), st.no_program.begin(), st.no_program.end());
        st.no_program.clear();
    }
    // Lowest priority: one prediction at a time, only while no draw, compute or translation job waits, and on at most
    // half of the workers (a draw's job may still find every worker busy for one build).
    bool queue_idle = false;
    if (!st.ready.empty() && st.inflight.size() < std::max<size_t>(1, async_workers.size() / 2)) {
        std::scoped_lock lock{async_mutex};
        queue_idle = job_queues.Empty();
        if (!queue_idle) {
            ++st.busy_waits;
        }
    }
    for (int n = 0; n < 1 && queue_idle && !st.ready.empty(); ++n) {
        const SotcPipePredict::Slot s = st.ready.front();
        st.ready.pop_front();
        const auto& tpl = st.templates[s.k];
        // A pass slot whose draws disagree on the rest of the key (per-material blend, several target sets) is
        // not predicted.
        if (st.conflict_k[s.k & 15] > 8) {
            ++st.unstable_skipped;
            continue;
        }
        // A program the boot hold translated is in the pack until its first draw: its modules are created now.
        for (const u64 h : {s.vs, s.ps}) {
            if (!program_cache.contains(h) && LoadLazyProgram(h)) {
                ++st.lazy_loaded;
            }
        }
        const auto vs_it = program_cache.find(s.vs);
        const auto ps_it = program_cache.find(s.ps);
        if (vs_it == program_cache.end() || vs_it->second->modules.empty()) {
            ++st.no_vs;
            st.no_program.push_back(s);
            continue;
        }
        if (ps_it == program_cache.end() || ps_it->second->modules.empty()) {
            ++st.no_ps;
            st.no_program.push_back(s);
            continue;
        }
        Program& vs = *vs_it.value();
        Program& ps = *ps_it.value();
        // The vertex variant: the first (with SOTC_VS_DRAW_KIND the only one of an instanced program). The pixel
        // variant: the one translated for this pass's render-target layout.
        const size_t vs_perm = 0;
        size_t ps_perm = ps.modules.size();
        for (size_t i = 0; i < ps.modules.size(); ++i) {
            const auto& fs = ps.modules[i].spec.runtime_info.hw.fs;
            if (std::ranges::equal(fs.color_buffers, tpl.color_buffers) && fs.mrtz_mask == tpl.mrtz_mask &&
                fs.num_samples == tpl.num_samples) {
                ps_perm = i;
                break;
            }
        }
        if (ps_perm == ps.modules.size()) {
            ++st.no_layout;
            continue;
        }
        GraphicsPipelineKey key = tpl.key;
        key.stage_hashes[u32(Shader::SwStage::Vertex)] = HashCombine(s.vs, u64{vs_perm});
        key.stage_hashes[u32(Shader::SwStage::Fragment)] = HashCombine(s.ps, u64{ps_perm});
        if (graphics_pipelines.contains(key)) {
            ++st.existing;
            continue;
        }
        // The draw's stage state is replaced for the job and put back (RefreshGraphicsKey runs next anyway).
        const auto saved_key = graphics_key;
        const auto saved_infos = infos;
        const auto saved_modules = modules;
        const auto saved_runtime = runtime_infos;
        auto* const saved_fetch = fetch_shader;
        graphics_key = key;
        infos.fill(nullptr);
        modules.fill(nullptr);
        infos[u32(Shader::SwStage::Vertex)] = &vs.info;
        infos[u32(Shader::SwStage::Fragment)] = &ps.info;
        modules[u32(Shader::SwStage::Vertex)] = vs.modules[vs_perm].module;
        modules[u32(Shader::SwStage::Fragment)] = ps.modules[ps_perm].module;
        runtime_infos[u32(Shader::SwStage::Vertex)] = vs.modules[vs_perm].spec.runtime_info;
        runtime_infos[u32(Shader::SwStage::Fragment)] = ps.modules[ps_perm].spec.runtime_info;
        auto& fetch = vs.modules[vs_perm].spec.fetch_shader_data;
        fetch_shader = fetch.Empty() ? nullptr : &fetch;
        if (AsyncPipelineEligible()) {
            graphics_pipelines.try_emplace(key); // null until collected, as for a draw's async build
            QueueAsyncPipeline(std::hash<GraphicsPipelineKey>{}(key), nullptr, false, true);
            st.predicted.insert(key);
            st.inflight.insert(key);
            ++st.queued;
        } else {
            ++st.ineligible;
        }
        graphics_key = saved_key;
        infos = saved_infos;
        modules = saved_modules;
        runtime_infos = saved_runtime;
        fetch_shader = saved_fetch;
    }
    const auto now = std::chrono::steady_clock::now();
    st.cp_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count());
    if (now - st.last_report >= std::chrono::seconds(5)) {
        st.last_report = now;
        const auto m = SotcPipePredict::Stats();
        size_t blocked = 0;
        for (const auto& b : st.blocked) {
            blocked += b.size();
        }
        std::fprintf(stderr,
                     "SOTC PIPE PREDICT: materials %llu (read %llu, no slot %llu), slots %llu (new pairs %llu, repeated "
                     "%llu, unreadable %llu), templates %llu, queued %llu, draws that found one %llu, draws that "
                     "built a known pair %llu; waiting: template %zu, program %zu, ready %zu; skipped: existing %llu, "
                     "no VS %llu, no PS %llu, no PS layout %llu, ineligible %llu; template conflicts %llu; programs "
                     "loaded from the pack %llu; command processor %.1f ms; built before their draw %llu, in "
                     "flight %zu, unstable slot %llu, worker queue busy %llu\n",
                     static_cast<unsigned long long>(m.queued), static_cast<unsigned long long>(m.read),
                     static_cast<unsigned long long>(m.empty), static_cast<unsigned long long>(m.slots),
                     static_cast<unsigned long long>(st.slots_new), static_cast<unsigned long long>(st.slots_dup),
                     static_cast<unsigned long long>(m.unreadable),
                     static_cast<unsigned long long>(st.templates_learned),
                     static_cast<unsigned long long>(st.queued), static_cast<unsigned long long>(st.hits),
                     static_cast<unsigned long long>(st.draw_new_known), blocked, st.no_program.size(),
                     st.ready.size(), static_cast<unsigned long long>(st.existing),
                     static_cast<unsigned long long>(st.no_vs), static_cast<unsigned long long>(st.no_ps),
                     static_cast<unsigned long long>(st.no_layout), static_cast<unsigned long long>(st.ineligible),
                     static_cast<unsigned long long>(st.template_conflicts),
                     static_cast<unsigned long long>(st.lazy_loaded), static_cast<double>(st.cp_ns) / 1e6,
                     static_cast<unsigned long long>(st.hits_built), st.inflight.size(),
                     static_cast<unsigned long long>(st.unstable_skipped),
                     static_cast<unsigned long long>(st.busy_waits));
        std::string fields;
        for (size_t f = 0; f < kPredictFields.size(); ++f) {
            if (st.conflict_fields[f]) {
                fields += fmt::format(" {} {},", kPredictFields[f].name, st.conflict_fields[f]);
            }
        }
        std::string ks;
        for (size_t k = 0; k < st.conflict_k.size(); ++k) {
            if (st.conflict_k[k]) {
                ks += fmt::format(" k{} {},", k, st.conflict_k[k]);
            }
        }
        std::fprintf(stderr, "SOTC PIPE PREDICT conflicts: fields%s; slots%s\n", fields.c_str(), ks.c_str());
    }
}

void PipelineCache::NotePredictDraw(bool is_new, bool built) {
    auto& st = *pipe_predict;
    if (!is_new && !st.predicted.empty() && st.predicted.erase(graphics_key) != 0) {
        ++st.hits;
        if (built) {
            ++st.hits_built;
        }
    }
    const auto* vs = infos[u32(Shader::SwStage::Vertex)];
    const auto* ps = infos[u32(Shader::SwStage::Fragment)];
    if (!vs || !ps) {
        return;
    }
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (i != u32(Shader::SwStage::Vertex) && i != u32(Shader::SwStage::Fragment) && infos[i]) {
            return; // VS+PS pipelines only
        }
    }
    const auto found = st.pair_k.find(PredictPairKey(vs->pgm_hash, ps->pgm_hash));
    if (found == st.pair_k.end()) {
        return;
    }
    const u8 k = found->second;
    if (is_new) {
        ++st.draw_new_known; // a draw built a pipeline of a known material slot: not predicted (yet)
    }
    GraphicsPipelineKey rest = graphics_key;
    rest.stage_hashes.fill(0);
    auto& tpl = st.templates[k];
    if (tpl.valid) {
        if (std::memcmp(&rest, &tpl.key, sizeof(rest)) != 0) {
            ++st.template_conflicts;
            ++st.conflict_k[k & 15];
            std::string which;
            const auto* a = reinterpret_cast<const u8*>(&rest);
            const auto* b = reinterpret_cast<const u8*>(&tpl.key);
            for (size_t f = 0; f + 1 < kPredictFields.size(); ++f) {
                const auto& fd = kPredictFields[f];
                if (std::memcmp(a + fd.begin, b + fd.begin, fd.end - fd.begin) != 0) {
                    ++st.conflict_fields[f];
                    which += fmt::format(" {}", fd.name);
                }
            }
            if (st.conflict_lines++ < 16) {
                std::fprintf(stderr, "SOTC PIPE PREDICT conflict: slot k %u, vs %#llx ps %#llx differs in%s\n",
                             unsigned{k}, static_cast<unsigned long long>(vs->pgm_hash),
                             static_cast<unsigned long long>(ps->pgm_hash), which.c_str());
            }
        }
        return;
    }
    const auto& fs = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
    tpl.key = rest;
    std::copy(fs.color_buffers.begin(), fs.color_buffers.end(), tpl.color_buffers.begin());
    tpl.mrtz_mask = fs.mrtz_mask;
    tpl.num_samples = fs.num_samples;
    tpl.valid = true;
    ++st.templates_learned;
    st.ready.insert(st.ready.end(), st.blocked[k].begin(), st.blocked[k].end());
    st.blocked[k].clear();
}

// sotc: SOTC_GPL_PREDICT. The state a pre-rasterization library is made of besides its stage and layout (pre_key in
// the GraphicsPipeline constructor), and the one of a per-shader fragment library (fs_key; the sample shading comes
// from the pixel program itself).
static u64 LibPreState(const GraphicsPipelineKey& k) {
    return u64{static_cast<u32>(k.prim_type)} | (u64{static_cast<u32>(k.polygon_mode)} << 8) |
           (u64{static_cast<u32>(k.provoking_vtx_last)} << 12) | (u64{k.depth_clamp_enable} << 16) |
           (u64{k.depth_clip_enable} << 17) | (u64{static_cast<u32>(k.clip_space)} << 18) |
           (u64{k.patch_control_points} << 32);
}
static u64 LibFsState(const GraphicsPipelineKey& k) {
    return u64{k.num_samples} | (u64{static_cast<u32>(k.z_format)} << 8) |
           (u64{static_cast<u32>(k.stencil_format)} << 12) | (u64{1} << 63);
}

// sotc: SOTC_GPL_PREDICT (nonblocking.h). Command processor only.
struct LibPredictState {
    /// The rest of a key (stage hashes zeroed), and the pixel layout of the draw it came from.
    struct Template {
        bool valid{};
        GraphicsPipelineKey rest{};
        std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers> color_buffers{};
        u32 mrtz_mask{};
        u32 num_samples{};
    };
    /// The last new key of a draw of each material pass slot k.
    std::array<Template, 16> by_slot{};
    /// The pre-rasterization and fragment states of the new keys and of the manifest's VS+PS pipelines, counted; the
    /// most common of each serves a slot whose pass gave no key yet. Node-based maps: the top pointers stay valid.
    struct Tally {
        u64 count{};
        GraphicsPipelineKey rest{};
    };
    std::unordered_map<u64, Tally> pre_tally, fs_tally;
    const Tally* pre_top{};
    const Tally* fs_top{};
    /// (VS hash, PS hash) of every material slot read -> its pass slot k.
    std::unordered_map<u64, u8> pair_k;
    /// Slots whose library jobs are not queued yet (a program not loaded, or no state known).
    struct Want {
        SotcPipePredict::Slot slot;
        std::chrono::steady_clock::time_point since;
    };
    std::deque<Want> waiting;
    /// Library jobs queued so far: HashCombine(stage hash, LibPreState / LibFsState).
    std::unordered_set<u64> queued;
    /// sotc: library ids whose predicted job is queued or running (never evicted meanwhile; CollectAsyncPipelines erases).
    std::unordered_set<u64> inflight;
    std::chrono::steady_clock::time_point last_evict{};
    std::vector<SotcPipePredict::Slot> scratch;
    u64 calls{};

    void Note(const GraphicsPipelineKey& rest) {
        const auto bump = [&rest](std::unordered_map<u64, Tally>& map, u64 id, const Tally*& top) {
            Tally& t = map[id];
            if (t.count++ == 0) {
                t.rest = rest;
            }
            if (!top || t.count > top->count) {
                top = &t;
            }
        };
        bump(pre_tally, LibPreState(rest), pre_top);
        bump(fs_tally, LibFsState(rest), fs_top);
    }
};

void PipelineCache::NoteLibraryTemplate() {
    auto& st = *lib_predict;
    const auto* vs = infos[u32(Shader::SwStage::Vertex)];
    const auto* ps = infos[u32(Shader::SwStage::Fragment)];
    if (!vs || !ps) {
        return;
    }
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (i != u32(Shader::SwStage::Vertex) && i != u32(Shader::SwStage::Fragment) && infos[i]) {
            return; // VS+PS pipelines only (the materials' slots)
        }
    }
    GraphicsPipelineKey rest = graphics_key;
    rest.stage_hashes.fill(0);
    st.Note(rest);
    const auto found = st.pair_k.find(PredictPairKey(vs->pgm_hash, ps->pgm_hash));
    if (found == st.pair_k.end() || found->second >= st.by_slot.size()) {
        return;
    }
    auto& tpl = st.by_slot[found->second];
    const auto& fs = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
    tpl.rest = rest;
    std::copy(fs.color_buffers.begin(), fs.color_buffers.end(), tpl.color_buffers.begin());
    tpl.mrtz_mask = fs.mrtz_mask;
    tpl.num_samples = fs.num_samples;
    tpl.valid = true;
}

// sotc: SOTC_COMPUTE_PACK. Bit 63 of a pack list id is its kind: 0 a graphics stage library, 1 a compute pipeline.
static constexpr u64 kPackKindCompute = u64{1} << 63;

// sotc: SOTC_GPL_DYN_STATE / SOTC_GPL_PACK_LIBS. The id of one stage library: program (CacheHash), permutation, part.
static u64 PackLibraryId(u64 program, size_t perm, u32 part) {
    return HashCombine(HashCombine(program, u64{perm}), u64{0x9000u + part}) & ~kPackKindCompute;
}

// sotc: SOTC_COMPUTE_PACK. The id of one compute pipeline of the pack: its ComputePipelineKey (program CacheHash and
// permutation, HashCombine(hash, perm)) with the compute kind bit.
static u64 PackComputeId(u64 compute_key_value) {
    return HashCombine(compute_key_value, u64{0xC000u}) | kPackKindCompute;
}

// sotc: SOTC_GPL_DYN_STATE. The rest of a key for a library job that has no draw to take it from. With shader-only
// library keys none of it reaches the shader libraries; it only has to make a valid pipeline description.
static GraphicsPipelineKey DefaultLibraryRest() {
    GraphicsPipelineKey k;
    k.prim_type = AmdGpu::PrimitiveType::TriangleList;
    k.num_samples = 1;
    k.depth_samples = 1;
    k.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
    return k;
}

std::unique_ptr<PipelineCache::AsyncPipelineJob> PipelineCache::MakeStageLibraryJob(const GraphicsPipelineKey& rest,
                                                                                    Program& vs, size_t vs_perm,
                                                                                    Program& ps, size_t ps_perm,
                                                                                    u32 parts, u8 origin) {
    const auto vs_slot = u32(Shader::SwStage::Vertex);
    const auto ps_slot = u32(Shader::SwStage::Fragment);
    if (vs.info.sw_stage != Shader::SwStage::Vertex || ps.info.sw_stage != Shader::SwStage::Fragment ||
        &StageProfile(vs.info.variant) != &StageProfile(ps.info.variant) || vs_perm >= vs.modules.size() ||
        ps_perm >= ps.modules.size() || !vs.modules[vs_perm].module || !ps.modules[ps_perm].module) {
        return {};
    }
    for (const Program* p : {&vs, &ps}) {
        for (const auto& image : p->info.images) {
            if (image.array_size == 0 && image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                return {}; // its pipelines read draw state (AsyncIneligibleReason)
            }
        }
    }
    auto job = std::make_unique<AsyncPipelineJob>();
    job->key = rest;
    job->key.stage_hashes = {};
    job->key.stage_hashes[vs_slot] = HashCombine(vs.info.CacheHash(), u64{vs_perm});
    job->key.stage_hashes[ps_slot] = HashCombine(ps.info.CacheHash(), u64{ps_perm});
    job->hash = std::hash<GraphicsPipelineKey>{}(job->key);
    job->infos.fill(nullptr);
    job->modules.fill(vk::ShaderModule{});
    job->runtime_infos = runtime_infos;
    job->infos[vs_slot] = &vs.info;
    job->infos[ps_slot] = &ps.info;
    job->modules[vs_slot] = vs.modules[vs_perm].module;
    job->modules[ps_slot] = ps.modules[ps_perm].module;
    job->runtime_infos[vs_slot] = vs.modules[vs_perm].spec.runtime_info;
    job->runtime_infos[ps_slot] = ps.modules[ps_perm].spec.runtime_info;
    if (const auto& fetch = vs.modules[vs_perm].spec.fetch_shader_data; !fetch.Empty()) {
        job->fetch_shader = fetch;
    }
    if (!GraphicsPipeline::FastLinkable(job->key, job->infos, job->runtime_infos, StagesProfile(job->infos))) {
        return {};
    }
    job->link_only = true;
    job->warm_lib = true;
    job->lib_parts = static_cast<u8>(parts);
    job->lib_origin = origin;
    return job;
}

void PipelineCache::PredictLibraries(const std::vector<SotcPipePredict::Slot>* slots) {
    if (!instance.IsGraphicsPipelineLibrary() || instance.IsDescriptorHeapEnabled() ||
        !instance.IsVertexInputDynamicState() || EmulatorSettings.IsShaderCollect()) {
        return; // the library jobs read no draw state: only with dynamic vertex input, never with heap pipelines
    }
    if (!lib_predict) {
        lib_predict = std::make_shared<LibPredictState>();
    }
    auto& st = *lib_predict;
    ++st.calls;
    if (pack_ref.pack_done && pack_ref.done_count.load(std::memory_order_acquire) != 0) {
        DrainPackRef(); // sotc: SOTC_PACK_REFERENCED: the late libraries finished meanwhile
    }
    const auto now = std::chrono::steady_clock::now();
    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    // sotc: SOTC_GPL_PREDICT eviction (every 10 s during play): a predicted library no material referenced and no draw
    // used for 180 seconds leaves memory; its compiled code stays in the driver cache (a later material rebuilds it
    // from there), never one of the manifest's, never a job in flight. The handles are destroyed once the GPU has
    // finished the frames that linked them (DeferOperation).
    constexpr u64 evict_age_ns = 180ull * 1'000'000'000ull;
    if (!manifest_holding && !pre_holding && now - st.last_evict > std::chrono::seconds{10}) {
        st.last_evict = now;
        GplLibraryCache::Evicted evicted;
        if (gpl_libs.EvictStale(now_ns, evict_age_ns, st.inflight, evicted) != 0) {
            for (const u64 id : evicted.ids) {
                st.queued.erase(id); // a material naming it again queues a new job (a driver cache hit)
            }
            scheduler.DeferOperation([pipes = std::move(evicted.pipes)] { (void)pipes.size(); });
        }
    }
    if (!slots && SotcPipePredict::HasQueued()) {
        st.scratch.clear();
        SotcPipePredict::TakeSlots(st.scratch, 4);
        slots = &st.scratch;
    }
    bool fresh = false;
    if (slots) {
        for (const auto& s : *slots) {
            if (!st.pair_k.try_emplace(PredictPairKey(s.vs, s.ps), s.k).second) {
                continue; // a pair another material named first
            }
            g_nodrop.pred_slots.fetch_add(1, std::memory_order_relaxed);
            if (st.waiting.size() >= 8192) {
                st.waiting.pop_front();
                g_nodrop.pred_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            st.waiting.push_back({s, now});
            fresh = true;
        }
    }
    // Pack translations held back while library jobs ran (FeedPretranslate) go on once they are done.
    if ((st.calls & 63) == 0 && !pre_backlog.empty() && lib_jobs_pending.load(std::memory_order_relaxed) == 0) {
        FeedPretranslate();
    }
    // sotc: SOTC_GPL_DYN_STATE: with shader-only library keys a slot needs no pass state (a library is the same for
    // every draw of its stage permutation), and during a manifest hold every waiting slot is looked at on each call.
    const bool shader_keys = SotcNonBlocking::GplShaderKeys();
    if (st.waiting.empty() || (!fresh && (st.calls & 15) != 0 && !manifest_holding)) {
        g_nodrop.pred_waiting.store(st.waiting.size(), std::memory_order_relaxed);
        return;
    }
    // A few slots per call: each queues its jobs, goes back to the end of the line, or is given up.
    for (size_t n = std::min<size_t>(st.waiting.size(), manifest_holding ? st.waiting.size() : 4); n > 0; --n) {
        const LibPredictState::Want w = st.waiting.front();
        st.waiting.pop_front();
        const auto& s = w.slot;
        const auto retry = [&] {
            if (now - w.since > std::chrono::seconds{60}) {
                g_nodrop.pred_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                st.waiting.push_back(w);
            }
        };
        const LibPredictState::Template* tpl =
            s.k < st.by_slot.size() && st.by_slot[s.k].valid ? &st.by_slot[s.k] : nullptr;
        if (!tpl && (!st.pre_top || !st.fs_top) && !shader_keys) {
            g_nodrop.pred_no_template.fetch_add(1, std::memory_order_relaxed);
            retry();
            continue;
        }
        // Both programs in the program cache: a program still in the pack is loaded by a worker, a pack translation
        // still waiting goes to the front of its backlog.
        bool ready = true;
        for (const u64 h : {s.vs, s.ps}) {
            if (program_cache.contains(h)) {
                continue;
            }
            ready = false;
            if (lazy_programs.contains(h)) {
                if (QueueLazyPrefetch(h)) {
                    g_nodrop.pred_pack_loads.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (pre_pending.contains(h)) {
                const auto f = std::find_if(pre_backlog.begin(), pre_backlog.end(), [h](const auto& job) {
                    return job->translate && job->translate->hash == h;
                });
                if (f != pre_backlog.end() && f != pre_backlog.begin()) {
                    auto job = std::move(*f);
                    pre_backlog.erase(f);
                    pre_backlog.push_front(std::move(job));
                    g_nodrop.pred_promoted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (!ready) {
            g_nodrop.pred_no_program.fetch_add(1, std::memory_order_relaxed);
            retry();
            continue;
        }
        Program& vs = *program_cache.find(s.vs)->second;
        Program& ps = *program_cache.find(s.ps)->second;
        bool linkable = vs.info.sw_stage == Shader::SwStage::Vertex && ps.info.sw_stage == Shader::SwStage::Fragment &&
                        &StageProfile(vs.info.variant) == &StageProfile(ps.info.variant);
        for (const Program* p : {&vs, &ps}) {
            for (const auto& image : p->info.images) {
                linkable &= !(image.array_size == 0 &&
                              image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex);
            }
        }
        const auto first_perm = [](const Program& p) {
            for (size_t i = 0; i < p.modules.size(); ++i) {
                if (p.modules[i].module) {
                    return i;
                }
            }
            return p.modules.size();
        };
        const size_t vs0 = first_perm(vs), ps0 = first_perm(ps);
        if (!linkable || vs0 == vs.modules.size() || ps0 == ps.modules.size()) {
            g_nodrop.pred_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const GraphicsPipelineKey default_rest = DefaultLibraryRest();
        const GraphicsPipelineKey& pre_rest = tpl ? tpl->rest : st.pre_top ? st.pre_top->rest : default_rest;
        const GraphicsPipelineKey& fs_rest = tpl ? tpl->rest : st.fs_top ? st.fs_top->rest : default_rest;
        // One job per permutation and part: the vertex permutations with the slot's first pixel permutation (their
        // pre-rasterization library), the pixel permutations with the first vertex one (their fragment library). A
        // library is keyed by its stage hash, so a draw of another permutation (another specialization) never finds it.
        // sotc: SOTC_PACK_REFERENCED: after the first-loading pack, a material's libraries are late (listed once built).
        const bool late_refs = pack_ref.pack_done && shader_keys && SotcNonBlocking::GplPackReferenced();
        const auto queue = [&](const GraphicsPipelineKey& rest, size_t vs_perm, size_t ps_perm, u32 parts, u64 id,
                               u64 stage_program) {
            if (shader_keys) {
                gpl_libs.Touch(id, now_ns); // sotc: this material references the library (eviction by age)
            }
            if (!st.queued.insert(id).second) {
                return; // queued before (another material of the same program, same state)
            }
            auto job = MakeStageLibraryJob(rest, vs, vs_perm, ps, ps_perm, parts, GplLibraryCache::OriginPredicted);
            if (!job) {
                g_nodrop.pred_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            job->pack_id = id; // sotc: the library's id (GplLibraryCache::SetLibraryId in RunLibraryJob)
            if (late_refs) { // sotc: SOTC_PACK_REFERENCED: noted when built, listed for the next launch
                job->late_pack = true;
                job->pack_program = stage_program;
                if (pack_ref.watch.try_emplace(stage_program, false).second) {
                    ++pack_ref.undrawn;
                }
                ++pack_ref.queued;
            }
            st.inflight.insert(id);
            lib_jobs_pending.fetch_add(1, std::memory_order_relaxed);
            g_nodrop.pred_jobs.fetch_add(1, std::memory_order_relaxed);
            {
                std::scoped_lock lock{async_mutex};
                PushJobLocked(JobLane::Predict, std::move(job));
            }
            async_cv.notify_one();
        };
        using L = GplLibraryCache;
        // sotc: SOTC_GPL_DYN_STATE: with shader-only keys only the shader part is built (the vertex input and output
        // libraries hold draw state; they are cheap and built at the first link), and a job is one per stage permutation.
        for (size_t i = 0; i < vs.modules.size(); ++i) {
            if (vs.modules[i].module) {
                queue(pre_rest, i, ps0,
                      shader_keys ? (1u << L::PreRaster) : (1u << L::PreRaster) | (1u << L::VertexInput),
                      shader_keys ? PackLibraryId(vs.info.CacheHash(), i, L::PreRaster)
                                  : HashCombine(HashCombine(s.vs, u64{i}), LibPreState(pre_rest)),
                  s.vs);
            }
        }
        // The pixel permutations translated for the pass's render-target layout when its pass is known (all when none
        // matches, or the pass gave no key yet).
        std::vector<size_t> ps_perms;
        for (size_t j = 0; j < ps.modules.size(); ++j) {
            const auto& fs = ps.modules[j].spec.runtime_info.hw.fs;
            if (ps.modules[j].module && tpl && !shader_keys && std::ranges::equal(fs.color_buffers, tpl->color_buffers) &&
                fs.mrtz_mask == tpl->mrtz_mask && fs.num_samples == tpl->num_samples) {
                ps_perms.push_back(j);
            }
        }
        if (ps_perms.empty()) {
            for (size_t j = 0; j < ps.modules.size(); ++j) {
                if (ps.modules[j].module) {
                    ps_perms.push_back(j);
                }
            }
        }
        for (const size_t j : ps_perms) {
            queue(fs_rest, vs0, j,
                  shader_keys ? (1u << L::Fragment) : (1u << L::Fragment) | (1u << L::FragmentOutput),
                  shader_keys ? PackLibraryId(ps.info.CacheHash(), j, L::Fragment)
                              : HashCombine(HashCombine(s.ps, u64{j}), LibFsState(fs_rest)),
              s.ps);
        }
    }
    g_nodrop.pred_waiting.store(st.waiting.size(), std::memory_order_relaxed);
}

std::unique_ptr<PipelineCache::AsyncPipelineJob> PipelineCache::MakeLibraryJob(const GraphicsPipelineKey& key,
                                                                               u32 parts, u8 origin) {
    auto job = std::make_unique<AsyncPipelineJob>();
    job->key = key;
    job->hash = std::hash<GraphicsPipelineKey>{}(key);
    job->infos = infos;
    job->runtime_infos = runtime_infos;
    job->modules = modules;
    if (fetch_shader && !fetch_shader->Empty()) {
        job->fetch_shader = *fetch_shader;
    }
    job->link_only = true;
    job->warm_lib = true;
    job->lib_parts = static_cast<u8>(parts);
    job->lib_origin = origin;
    return job;
}

void PipelineCache::RunLibraryJob(AsyncPipelineJob& job) {
    const SotcTrace::Scope trace{"gpl library job", job.hash, job.lib_origin}; // sotc: worker or CP
    // sotc: SOTC_GPL_PACK_LIBS: a pack job's library is compiled into the driver cache and destroyed.
    const bool pack = job.lib_origin == GplLibraryCache::OriginPack;
    const u64 built0 = GplLibraryCache::ThreadBuilt(), found0 = GplLibraryCache::ThreadFound(),
              failed0 = GplLibraryCache::ThreadFailed();
    GplLibraryCache::SetDiscard(pack);
    GplLibraryCache::SetLibrariesOnly(true);
    GplLibraryCache::SetLibraryParts(job.lib_parts);
    GplLibraryCache::SetOrigin(static_cast<GplLibraryCache::Origin>(job.lib_origin));
    GplLibraryCache::SetLibraryId(job.pack_id); // sotc: SOTC_GPL_PREDICT (0 for a draw's urgent job)
    // The constructor builds the parts of the mask, looks the others up and stops before the link (no handle).
    job.pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, layout_cache, StagesProfile(job.infos), job.key, *pipeline_cache, job.infos,
        job.runtime_infos, job.fetch_shader ? &*job.fetch_shader : nullptr, job.modules, job.sdata, false, true,
        &gpl_libs);
    job.pipeline.reset();
    GplLibraryCache::SetLibraryId(0);
    GplLibraryCache::SetOrigin(GplLibraryCache::OriginDefault);
    GplLibraryCache::SetLibraryParts(0);
    GplLibraryCache::SetLibrariesOnly(false);
    GplLibraryCache::SetDiscard(false);
    if (pack) {
        // Compiled (or already stored, hence in the driver cache too) when a part was built or found and none refused.
        const bool ok = GplLibraryCache::ThreadFailed() == failed0 &&
                        (GplLibraryCache::ThreadBuilt() != built0 || GplLibraryCache::ThreadFound() != found0);
        if (ok) {
            std::scoped_lock lock{pack_mutex};
            pack_compiled.push_back(job.pack_id);
            if (SotcNonBlocking::GplPackLean()) { // sotc: the command processor destroys the module (DrainPackModules)
                pack_done_modules.emplace_back(job.pack_program, job.pack_perm);
            }
        } else {
            pack_failed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (job.late_pack) { // sotc: SOTC_PACK_REFERENCED: counted and listed by the command processor (DrainPackRef)
        const bool ok = GplLibraryCache::ThreadFailed() == failed0 &&
                        (GplLibraryCache::ThreadBuilt() != built0 || GplLibraryCache::ThreadFound() != found0);
        std::scoped_lock lock{pack_mutex};
        pack_ref.done.push_back({job.pack_id, job.pack_program, ok});
        pack_ref.done_count.fetch_add(1, std::memory_order_release);
    }
    lib_jobs_pending.fetch_sub(1, std::memory_order_relaxed);
    if (job.lib_wait) {
        {
            std::scoped_lock lock{async_mutex};
            job.lib_wait->remaining.fetch_sub(1, std::memory_order_relaxed);
        }
        lib_wait_cv.notify_all();
    }
}

bool PipelineCache::BuildLibrariesParallel(u32 missing) {
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::LibraryWait,
                                       std::hash<GraphicsPipelineKey>{}(graphics_key)}; // sotc: includes self-build
    using L = GplLibraryCache;
    constexpr u32 pre = 1u << L::PreRaster;
    constexpr u32 fs = 1u << L::Fragment;
    if ((missing & pre) == 0 || (missing & fs) == 0 || async_workers.empty()) {
        return false; // one shader library (or none) missing: the link builds it on this thread
    }
    const auto start = std::chrono::steady_clock::now();
    const auto group = std::make_shared<AsyncPipelineJob::LibWait>();
    group->remaining.store(2, std::memory_order_relaxed);
    auto vs_job = MakeLibraryJob(graphics_key, pre | (missing & (1u << L::VertexInput)), L::OriginUrgent);
    auto fs_job = MakeLibraryJob(graphics_key, fs | (missing & (1u << L::FragmentOutput)), L::OriginUrgent);
    vs_job->lib_wait = group;
    fs_job->lib_wait = group;
    lib_jobs_pending.fetch_add(2, std::memory_order_relaxed);
    {
        std::scoped_lock lock{async_mutex};
        auto& q = job_queues[JobLane::Urgent];
        q.push_front(std::move(fs_job));
        q.push_front(std::move(vs_job));
    }
    async_cv.notify_all();
    urgent_cv.notify_all(); // sotc: SOTC_GPL_URGENT_WORKERS
    // A job no worker has taken yet is run here (every worker may be in a long build).
    for (;;) {
        std::unique_ptr<AsyncPipelineJob> mine;
        {
            std::scoped_lock lock{async_mutex};
            auto& q = job_queues[JobLane::Urgent];
            const auto f = std::find_if(q.begin(), q.end(), [&group](const auto& j) { return j->lib_wait == group; });
            if (f != q.end()) {
                mine = std::move(*f);
                q.erase(f);
            }
        }
        if (!mine) {
            break;
        }
        RunLibraryJob(*mine);
        g_nodrop.par_taken.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::unique_lock lock{async_mutex};
        lib_wait_cv.wait_for(lock, std::chrono::seconds{10},
                             [&group] { return group->remaining.load(std::memory_order_relaxed) <= 0; });
    }
    g_nodrop.par_waits.fetch_add(1, std::memory_order_relaxed);
    g_nodrop.par_wait_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                        std::chrono::steady_clock::now() - start)
                                                        .count()),
                                   std::memory_order_relaxed);
    return true;
}

u64 SotcJobClockNs() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

namespace {
// sotc: SOTC_BLOCK_LEDGER job queue report (non-blocking frame P2.0): per job class, how long jobs waited between
// being made and a worker taking them, and how long they ran. Workers add; the command processor prints every 5 s.
enum JobClass : u8 {
    JobCompute, JobComputePrebuild, JobTranslate, JobPretranslate, JobTiny, JobShadow, JobGraphics, JobPreload,
    JobLazy, JobPredict, JobLink, JobWarm, JobClassCount
};
constexpr std::array<const char*, JobClassCount> kJobClassNames{
    "compute", "compute prebuild", "translate", "pretranslate", "tiny", "shadow", "graphics", "preload", "pack load",
    "predicted", "gpl link", "gpl warm",
};
// sotc: SOTC_NB_SCHED counters, printed with the job queue report.
std::atomic<u64> g_sched_cancelled{0}, g_sched_demand{0};

// sotc: SOTC_NB_SCHED: a pipeline worker running a background job yields to the game's threads.
void SetWorkerBelowNormal(bool below) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), below ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#else
    (void)below;
#endif
}
struct JobClassStats {
    std::atomic<u64> count{0}, wait_ns{0}, wait_max_ns{0}, run_ns{0}, run_max_ns{0};
};
std::array<JobClassStats, JobClassCount> g_job_stats;

void AtomicMax(std::atomic<u64>& a, u64 v) {
    u64 cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void NoteJob(JobClass c, u64 wait_ns, u64 run_ns) {
    auto& s = g_job_stats[c];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
    s.run_ns.fetch_add(run_ns, std::memory_order_relaxed);
    AtomicMax(s.wait_max_ns, wait_ns);
    AtomicMax(s.run_max_ns, run_ns);
}

void ReportJobs() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    last = now;
    std::string line = "SOTC JOB QUEUE (5 s; n, wait avg/max ms, run avg/max ms):";
    char buf[160];
    for (u32 c = 0; c < JobClassCount; ++c) {
        auto& s = g_job_stats[c];
        const u64 n = s.count.exchange(0, std::memory_order_relaxed);
        const u64 w = s.wait_ns.exchange(0, std::memory_order_relaxed);
        const u64 wm = s.wait_max_ns.exchange(0, std::memory_order_relaxed);
        const u64 r = s.run_ns.exchange(0, std::memory_order_relaxed);
        const u64 rm = s.run_max_ns.exchange(0, std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        std::snprintf(buf, sizeof buf, " %s %llu, %.1f/%.1f, %.1f/%.1f;", kJobClassNames[c],
                      static_cast<unsigned long long>(n), w / 1e6 / n, wm / 1e6, r / 1e6 / n, rm / 1e6);
        line += buf;
    }
    if (SotcNonBlocking::Sched()) {
        std::snprintf(buf, sizeof buf, " scheduler: skipped-draw demand %llu, cancelled builds %llu;",
                      static_cast<unsigned long long>(g_sched_demand.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_sched_cancelled.exchange(0, std::memory_order_relaxed)));
        line += buf;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}
} // namespace

void PipelineCache::QueueAsyncPipeline(u64 hash,
                                       const GraphicsPipeline::SerializationSupport* preloaded,
                                       bool priority, bool background, bool addr_table, bool front) {
    auto job = std::make_unique<AsyncPipelineJob>();
    job->addr_table = addr_table; // sotc: SOTC_ADDR_FALLBACK
    job->key = graphics_key;
    job->hash = hash;
    if (preloaded) {
        job->preloaded = true;
        job->sdata = *preloaded; // the cached vertex layout the pipeline is built from
        job->uses = warm_uses; // sotc: N5, the blob's use count
        g_n5.boot_total.fetch_add(1, std::memory_order_relaxed);
    }
    job->infos = infos;
    job->runtime_infos = runtime_infos;
    if (fetch_shader) {
        job->fetch_shader = *fetch_shader;
    }
    job->modules = modules;
    job->priority = priority;
    // sotc: SOTC_NB_SCHED: the skipped draws of this key raise its demand; a pipeline the command processor builds
    // itself cancels it.
    if (SotcNonBlocking::Sched() && !preloaded && !background) {
        queued_graphics[graphics_key] = job.get();
    }
    {
        std::scoped_lock lock{async_mutex};
        // sotc: shadow (depth-only) pipelines go ahead of the other graphics pipelines (after the compute and
        // translation jobs, which the recorder or a draw waits for); predicted ones go to the background.
        const JobLane lane = priority ? JobLane::Shadow : background ? JobLane::Background : JobLane::Graphics;
        if (front) {
            job_queues[lane].push_front(std::move(job)); // sotc: a draw waits for it (ahead of the preload)
        } else {
            PushJobLocked(lane, std::move(job));
        }
    }
    async_cv.notify_one();
}

void PipelineCache::QueueLinkJob() {
    auto job = std::make_unique<AsyncPipelineJob>();
    job->key = graphics_key;
    job->hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
    job->infos = infos;
    job->runtime_infos = runtime_infos;
    if (fetch_shader) {
        job->fetch_shader = *fetch_shader;
    }
    job->modules = modules;
    job->link_only = true;
    link_pending.insert(graphics_key);
    {
        std::scoped_lock lock{async_mutex};
        PushJobLocked(JobLane::Translate, std::move(job)); // draws are skipped until it is linked
    }
    async_cv.notify_one();
}

namespace {
// sotc: the tiny pipeline list lives next to the versioned cache folders, so it survives a port or driver update (the
// cache of a new version starts empty). Records: GraphicsPipelineKey bytes, then the vertex and pixel program hash and
// permutation. A file made with another key size is ignored.
constexpr u32 kTinyMagic = 0x594E4954; // "TINY"
std::filesystem::path TinyListPath() {
    return Storage::DataBase::Instance().CachePath().parent_path() / "sotc_tiny_pipelines.bin";
}
std::optional<size_t> PermOfStage(u64 stage_hash, u64 hash) {
    for (size_t p = 0; p < Program::MaxPermutations; ++p) {
        if (HashCombine(hash, u64{p}) == stage_hash) {
            return p;
        }
    }
    return std::nullopt;
}
} // namespace

void PipelineCache::RecordTinyPipeline() {
    const auto* vs = infos[u32(Shader::SwStage::Vertex)];
    const auto* ps = infos[u32(Shader::SwStage::Fragment)];
    if (!vs || !ps || tiny_list.contains(graphics_key)) {
        return;
    }
    const auto vp = PermOfStage(graphics_key.stage_hashes[u32(Shader::SwStage::Vertex)], vs->CacheHash());
    const auto pp = PermOfStage(graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)], ps->CacheHash());
    if (!vp || !pp) {
        return;
    }
    tiny_list.emplace(graphics_key, TinyEntry{vs->CacheHash(), ps->CacheHash(), static_cast<u32>(*vp),
                                              static_cast<u32>(*pp)});
    // A few KB, written by a detached thread (no file I/O on the command processor).
    std::vector<u8> data;
    const auto put = [&data](const void* p, size_t n) {
        data.insert(data.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
    };
    const u32 header[3] = {kTinyMagic, static_cast<u32>(sizeof(GraphicsPipelineKey)),
                           static_cast<u32>(tiny_list.size())};
    put(header, sizeof header);
    for (const auto& [key, e] : tiny_list) {
        put(&key, sizeof key);
        put(&e, sizeof e);
    }
    std::thread([path = TinyListPath(), data = std::move(data)] {
        auto tmp = path;
        tmp += ".tmp";
        if (std::FILE* f = _wfopen(tmp.c_str(), L"wb")) {
            const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            std::fclose(f);
            std::error_code ec;
            if (ok) {
                std::filesystem::rename(tmp, path, ec);
            } else {
                std::filesystem::remove(tmp, ec);
            }
        }
    }).detach();
}

void PipelineCache::BuildTinyLibraries() {
    if (!tiny_list_loaded) {
        tiny_list_loaded = true;
        if (std::FILE* f = _wfopen(TinyListPath().c_str(), L"rb")) {
            u32 header[3]{};
            if (std::fread(header, sizeof header, 1, f) == 1 && header[0] == kTinyMagic &&
                header[1] == sizeof(GraphicsPipelineKey)) {
                for (u32 i = 0; i < header[2]; ++i) {
                    GraphicsPipelineKey key;
                    TinyEntry e;
                    if (std::fread(&key, sizeof key, 1, f) != 1 || std::fread(&e, sizeof e, 1, f) != 1) {
                        break;
                    }
                    tiny_list.emplace(key, e);
                }
            }
            std::fclose(f);
        }
    }
    const u64 built_before = warm_built;
    u64 queued = 0;
    for (const auto& [key, e] : tiny_list) {
        const auto vs = program_cache.find(e.vs_hash);
        const auto ps = program_cache.find(e.ps_hash);
        if (vs == program_cache.end() || ps == program_cache.end() || e.vs_perm >= vs.value()->modules.size() ||
            e.ps_perm >= ps.value()->modules.size() || !vs.value()->modules[e.vs_perm].module ||
            !ps.value()->modules[e.ps_perm].module) {
            continue; // not loaded this session (yet): its first draw builds it as before
        }
        Program& v = *vs.value();
        Program& p = *ps.value();
        auto job = std::make_unique<AsyncPipelineJob>();
        job->key = key;
        job->hash = std::hash<GraphicsPipelineKey>{}(key);
        job->infos.fill(nullptr);
        job->modules.fill(vk::ShaderModule{});
        job->infos[u32(Shader::SwStage::Vertex)] = &v.info;
        job->infos[u32(Shader::SwStage::Fragment)] = &p.info;
        job->modules[u32(Shader::SwStage::Vertex)] = v.modules[e.vs_perm].module;
        job->modules[u32(Shader::SwStage::Fragment)] = p.modules[e.ps_perm].module;
        job->runtime_infos[u32(Shader::SwStage::Vertex)] = v.modules[e.vs_perm].spec.runtime_info;
        job->runtime_infos[u32(Shader::SwStage::Fragment)] = p.modules[e.ps_perm].spec.runtime_info;
        if (const auto& fetch = v.modules[e.vs_perm].spec.fetch_shader_data; !fetch.Empty()) {
            job->fetch_shader = fetch;
        }
        job->link_only = true;
        job->warm_lib = true;
        ++queued;
        {
            std::scoped_lock lock{async_mutex};
            PushJobLocked(JobLane::Translate, std::move(job));
        }
        async_cv.notify_one();
    }
    // The screen stays up until they are built (they use the hold's modules, which the eviction below destroys).
    const auto start = std::chrono::steady_clock::now();
    while (warm_built - built_before < queued && std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
        CollectAsyncPipelines();
        std::unique_lock lock{async_mutex};
        translate_done_cv.wait_for(lock, std::chrono::milliseconds{20},
                                   [this] { return async_done_count.load(std::memory_order_relaxed) != 0; });
    }
    std::fprintf(stderr, "SOTC NB TINY: %zu full-screen pipelines on the list, %llu built before play in %.1f s\n",
                 tiny_list.size(), static_cast<unsigned long long>(warm_built - built_before),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

void PipelineCache::NoteWarmTemplate() {
    const auto* vs = infos[u32(Shader::SwStage::Vertex)];
    const auto* ps = infos[u32(Shader::SwStage::Fragment)];
    if (!vs || !ps) {
        return;
    }
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (i != u32(Shader::SwStage::Vertex) && i != u32(Shader::SwStage::Fragment) && infos[i]) {
            return;
        }
    }
    const auto perm_of = [](u64 stage_hash, u64 hash) -> std::optional<size_t> {
        for (size_t p = 0; p < Program::MaxPermutations; ++p) {
            if (HashCombine(hash, u64{p}) == stage_hash) {
                return p;
            }
        }
        return std::nullopt;
    };
    const auto vp = perm_of(graphics_key.stage_hashes[u32(Shader::SwStage::Vertex)], vs->CacheHash());
    const auto pp = perm_of(graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)], ps->CacheHash());
    if (!vp || !pp) {
        return;
    }
    warm_template = {true, graphics_key, vs->CacheHash(), ps->CacheHash(), *vp, *pp};
}

void PipelineCache::FeedWarmLibraries() {
    // The log starts once the boot hold evicted the programs no draw used (their modules are destroyed there).
    if (!Program::new_perm_log) {
        if (!pre_hold_done || pre_holding) {
            return;
        }
        Program::new_perm_log = &warm_log;
        return;
    }
    if (!warm_template.valid || warm_log.empty()) {
        return;
    }
    const auto tpl_vs = program_cache.find(warm_template.vs_hash);
    const auto tpl_ps = program_cache.find(warm_template.ps_hash);
    if (tpl_vs == program_cache.end() || tpl_ps == program_cache.end() ||
        warm_template.vs_perm >= tpl_vs.value()->modules.size() ||
        warm_template.ps_perm >= tpl_ps.value()->modules.size()) {
        warm_template.valid = false; // the template's programs left: the next new draw gives another
        return;
    }
    // A few per draw: each is a job for a worker (background lane), the command processor only fills it.
    for (int n = 0; n < 4 && !warm_log.empty(); ++n) {
        const auto [hash, perm] = warm_log.back();
        warm_log.pop_back();
        if (!warm_done.insert(HashCombine(hash, u64{perm})).second) {
            continue;
        }
        const auto pgm = program_cache.find(hash);
        if (pgm == program_cache.end() || perm >= pgm.value()->modules.size() || !pgm.value()->modules[perm].module) {
            ++warm_skipped;
            continue;
        }
        Program& program = *pgm.value();
        const auto stage = program.info.sw_stage;
        if (stage != Shader::SwStage::Vertex && stage != Shader::SwStage::Fragment) {
            continue; // compute and the rest have no library
        }
        Program& vs = stage == Shader::SwStage::Vertex ? program : *tpl_vs.value();
        Program& ps = stage == Shader::SwStage::Fragment ? program : *tpl_ps.value();
        const size_t vs_perm = stage == Shader::SwStage::Vertex ? perm : warm_template.vs_perm;
        const size_t ps_perm = stage == Shader::SwStage::Fragment ? perm : warm_template.ps_perm;
        // sotc: SOTC_ADDR_FALLBACK: a table stage and a descriptor-mode stage never share a pipeline (StagesProfile).
        if (&StageProfile(vs.info.variant) != &StageProfile(ps.info.variant)) {
            ++warm_skipped;
            continue;
        }
        auto job = std::make_unique<AsyncPipelineJob>();
        job->key = warm_template.key;
        job->key.stage_hashes[u32(Shader::SwStage::Vertex)] = HashCombine(vs.info.CacheHash(), u64{vs_perm});
        job->key.stage_hashes[u32(Shader::SwStage::Fragment)] = HashCombine(ps.info.CacheHash(), u64{ps_perm});
        job->hash = std::hash<GraphicsPipelineKey>{}(job->key);
        job->infos.fill(nullptr);
        job->modules.fill(vk::ShaderModule{});
        job->infos[u32(Shader::SwStage::Vertex)] = &vs.info;
        job->infos[u32(Shader::SwStage::Fragment)] = &ps.info;
        job->modules[u32(Shader::SwStage::Vertex)] = vs.modules[vs_perm].module;
        job->modules[u32(Shader::SwStage::Fragment)] = ps.modules[ps_perm].module;
        job->runtime_infos[u32(Shader::SwStage::Vertex)] = vs.modules[vs_perm].spec.runtime_info;
        job->runtime_infos[u32(Shader::SwStage::Fragment)] = ps.modules[ps_perm].spec.runtime_info;
        if (const auto& fetch = vs.modules[vs_perm].spec.fetch_shader_data; !fetch.Empty()) {
            job->fetch_shader = fetch;
        }
        job->link_only = true;
        job->warm_lib = true;
        ++warm_queued;
        {
            std::scoped_lock lock{async_mutex};
            PushJobLocked(JobLane::Background, std::move(job));
        }
        async_cv.notify_one();
    }
    static auto last = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(5)) {
        last = now;
        std::fprintf(stderr,
                     "SOTC NB WARM: library warm-up jobs queued %llu, done %llu, skipped %llu, waiting %zu\n",
                     static_cast<unsigned long long>(warm_queued), static_cast<unsigned long long>(warm_built),
                     static_cast<unsigned long long>(warm_skipped), warm_log.size());
    }
}

u32 PipelineCache::BackgroundCap() const {
    // sotc: SOTC_NB_SCHED: background jobs (pretranslation, pack loads, prebuilds, prediction) on at most half the
    // workers while the game runs; the boot hold waits for them, so it gives them every worker.
    if (!SotcNonBlocking::Sched() || pre_holding) {
        return std::numeric_limits<u32>::max();
    }
    return std::max<u32>(1, static_cast<u32>(async_workers.size()) / 2);
}

u32 PipelineCache::CappedCap() const {
    // sotc: SOTC_NB_WORKER_CAP: the capped lanes (CappedLane) run on a quarter of the hardware threads, at least 2.
    // The boot holds (SOTC_PRETRANSLATE, SOTC_PIPE_MANIFEST) wait for them, so they give them every worker.
    if (!SotcNonBlocking::WorkerCap() || pre_holding || manifest_holding) {
        return std::numeric_limits<u32>::max();
    }
    return std::max<u32>(2, std::thread::hardware_concurrency() / 4);
}

void PipelineCache::PushJobLocked(JobLane lane, std::unique_ptr<AsyncPipelineJob> job) {
    job_queues[lane].push_back(std::move(job));
    if (UrgentLane(lane)) {
        urgent_cv.notify_one(); // sotc: SOTC_GPL_URGENT_WORKERS (the caller notifies async_cv for the others)
    }
}

bool PipelineCache::HoldCapActive() const {
    return CompileThreadCap() != 0 && compile_holds.load(std::memory_order_acquire) > 0;
}

void PipelineCache::SetCompileHold(bool active, const char* what) {
    LogRam(what); // sotc: SOTC RAM at each hold start and end
    if (CompileThreadCap() == 0 || async_workers.empty()) {
        return;
    }
    {
        std::scoped_lock lock{async_mutex};
        compile_holds.fetch_add(active ? 1 : -1, std::memory_order_acq_rel);
    }
    if (active) {
        std::fprintf(stderr,
                     "SOTC COMPILE THREADS: %s: at most %u compile jobs at once (of %zu workers), below normal "
                     "priority\n",
                     what, CompileThreadCap(), async_workers.size());
    } else {
        // The cap is lifted (or stays for an enclosing hold): the workers it held back look again.
        async_cv.notify_all();
        urgent_cv.notify_all();
    }
}

bool PipelineCache::HasRunnableJobLocked(bool urgent_only) const {
    // sotc: SOTC_COMPILE_THREADS: during a loading hold, nothing more runs while the cap's jobs are running.
    if (HoldCapActive() && job_queues.hold_running >= CompileThreadCap()) {
        return false;
    }
    for (size_t l = 0; l < static_cast<size_t>(JobLane::Count); ++l) {
        if (job_queues.lanes[l].empty()) {
            continue;
        }
        if (urgent_only && !UrgentLane(static_cast<JobLane>(l))) {
            continue; // sotc: SOTC_GPL_URGENT_WORKERS
        }
        if (static_cast<JobLane>(l) == JobLane::Background && job_queues.background_running >= BackgroundCap()) {
            continue;
        }
        // sotc: SOTC_NB_WORKER_CAP: a capped lane waits while its quota of workers is busy.
        if (CappedLane(static_cast<JobLane>(l)) && job_queues.capped_running >= CappedCap()) {
            continue;
        }
        return true;
    }
    return false;
}

std::unique_ptr<PipelineCache::AsyncPipelineJob> PipelineCache::PopJobLocked(JobLane& lane, bool urgent_only,
                                                                         bool* hold_slot) {
    // sotc: SOTC_COMPILE_THREADS: the hold cap (checked before any count), as in HasRunnableJobLocked.
    const bool hold_cap = HoldCapActive();
    if (hold_cap && job_queues.hold_running >= CompileThreadCap()) {
        return nullptr;
    }
    for (size_t l = 0; l < static_cast<size_t>(JobLane::Count); ++l) {
        auto& q = job_queues.lanes[l];
        if (q.empty()) {
            continue;
        }
        lane = static_cast<JobLane>(l);
        if (urgent_only && !UrgentLane(lane)) {
            continue; // sotc: SOTC_GPL_URGENT_WORKERS
        }
        // sotc: SOTC_NB_WORKER_CAP: a capped lane waits while its quota of workers is busy (checked before any count).
        if (CappedLane(lane) && job_queues.capped_running >= CappedCap()) {
            continue;
        }
        if (lane == JobLane::Background) {
            if (job_queues.background_running >= BackgroundCap()) {
                continue;
            }
            ++job_queues.background_running;
        }
        if (CappedLane(lane)) {
            ++job_queues.capped_running; // sotc: released by CappedSlot in AsyncPipelineWorker
        }
        auto pick = q.begin();
        // sotc: SOTC_NB_SCHED: the graphics job most draws are waiting for, aged so that none waits forever
        // (one skipped draw counts as 10 ms of age).
        if (lane == JobLane::Graphics && SotcNonBlocking::Sched() && q.size() > 1) {
            const u64 now = SotcJobClockNs();
            u64 best = 0;
            for (auto it = q.begin(); it != q.end(); ++it) {
                const u64 age_ms = (now - std::min(now, (*it)->created_ns)) / 1'000'000;
                const u64 score = u64{(*it)->demand.load(std::memory_order_relaxed)} * 10 + age_ms;
                if (it == q.begin() || score > best) {
                    best = score;
                    pick = it;
                }
            }
        }
        auto job = std::move(*pick);
        q.erase(pick);
        if (hold_cap) {
            ++job_queues.hold_running; // sotc: SOTC_COMPILE_THREADS: released by HoldSlot in AsyncPipelineWorker
            if (hold_slot) {
                *hold_slot = true;
            }
        }
        return job;
    }
    return nullptr;
}

void PipelineCache::AsyncPipelineWorker(std::stop_token token, bool urgent_only, bool extra) {
    Common::SetCurrentThreadName(urgent_only ? "shadPS4:PipelineUrgent" : extra ? "shadPS4:PipelinePack"
                                                                                 : "shadPS4:PipelineWorker");
    // sotc: allocate the trace ring before any shader, resource fault or ledger scope runs.
    SotcTrace::ThreadInit();
    // sotc: up to 12 workers compile while new areas stream in; below normal they no longer take
    // CPU time from the command processor, draw and game threads. Off by default (review of
    // 2026-10-27): the recorder (high priority) blocks on compute pipelines a worker claimed, and
    // skipped draws wait for translations, so starved workers turned into stalls exactly when the
    // CPU is busiest (streaming). SOTC_WORKER_LOW_PRIO=1 turns it on.
    static const bool low_prio = [] {
        const char* v = std::getenv("SOTC_WORKER_LOW_PRIO");
        return v && v[0] == '1';
    }();
    if (low_prio) {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    }
    BbThreads::PlaceThread(BbThreads::Role::PipelineWorker);
    for (;;) {
        std::unique_ptr<AsyncPipelineJob> job;
        JobClass job_class = JobGraphics;
        JobLane lane = JobLane::Graphics;
        bool hold_job = false; // sotc: SOTC_COMPILE_THREADS: counted against the hold cap (PopJobLocked)
        {
            std::unique_lock lock{async_mutex};
            // sotc: SOTC_GPL_URGENT_WORKERS: a reserved worker waits on urgent_cv (notified by pushes to its lanes);
            // SOTC_GPL_PACK_LEAN: an extra worker of the pack hold leaves when told, without draining anything.
            auto& cv = urgent_only ? urgent_cv : async_cv;
            cv.wait(lock, token, [this, urgent_only, extra] {
                return HasRunnableJobLocked(urgent_only) ||
                       (extra && extra_workers_quit.load(std::memory_order_acquire));
            });
            if (extra && extra_workers_quit.load(std::memory_order_acquire)) {
                return;
            }
            if (token.stop_requested()) {
                // A queued compute pipeline may be waited for by the recording thread: build
                // those before leaving.
                for (auto& q : job_queues.lanes) {
                    while (!q.empty()) {
                        job = std::move(q.front());
                        q.pop_front();
                        if (job->compute && !job->compute_opt) { // sotc: an optimized build has a fast one bound
                            job->compute->Build();
                        }
                    }
                }
                return;
            }
            job = PopJobLocked(lane, urgent_only, &hold_job);
            if (!job) {
                continue;
            }
            job_class = job->warm_lib            ? JobWarm
                        : job->link_only           ? JobLink
                        : job->lazy                ? JobLazy
                        : job->compute           ? (job->pre_compute ? JobComputePrebuild : JobCompute)
                        : job->translate         ? (job->translate->pretranslate ? JobPretranslate : JobTranslate)
                        : lane == JobLane::Tiny  ? JobTiny
                        : job->priority          ? JobShadow
                        : job->preloaded         ? JobPreload
                        : lane == JobLane::Background ? JobPredict
                                                 : JobGraphics;
        }
        // sotc: SOTC_NB_SCHED: a background job runs below normal priority, and frees its slot when done.
        struct BackgroundSlot {
            PipelineCache& cache;
            bool active;
            BackgroundSlot(PipelineCache& c, bool a) : cache(c), active(a) {
                if (active && SotcNonBlocking::Sched()) {
                    SetWorkerBelowNormal(true);
                }
            }
            ~BackgroundSlot() {
                if (!active) {
                    return;
                }
                if (SotcNonBlocking::Sched()) {
                    SetWorkerBelowNormal(false);
                }
                {
                    std::scoped_lock lock{cache.async_mutex};
                    --cache.job_queues.background_running;
                }
                cache.async_cv.notify_one();
            }
        } background_slot{*this, lane == JobLane::Background};
        // sotc: SOTC_NB_WORKER_CAP: a job of a capped lane runs below normal priority (when the cap is on) and frees its
        // quota slot (capped_running, counted by PopJobLocked) in every exit path, since the destructor runs at the end
        // of this loop iteration, including the early `continue`s and the cancelled-graphics path.
        struct CappedSlot {
            PipelineCache& cache;
            bool active;
            CappedSlot(PipelineCache& c, bool a) : cache(c), active(a) {
                if (active && SotcNonBlocking::WorkerCap()) {
                    SetWorkerBelowNormal(true);
                }
            }
            ~CappedSlot() {
                if (!active) {
                    return;
                }
                if (SotcNonBlocking::WorkerCap()) {
                    SetWorkerBelowNormal(false);
                }
                {
                    std::scoped_lock lock{cache.async_mutex};
                    --cache.job_queues.capped_running;
                }
                cache.async_cv.notify_all();
            }
        } capped_slot{*this, CappedLane(lane)};
        // sotc: SOTC_COMPILE_THREADS: a job taken during a loading hold runs below normal priority (the game's loader
        // threads and the loading screen first; the command processor waits through the hold) and frees its hold slot
        // in every exit path. The priority the worker had before is restored (normal for play).
        struct HoldSlot {
            PipelineCache& cache;
            bool active;
            int previous{};
            HoldSlot(PipelineCache& c, bool a) : cache(c), active(a) {
#ifdef _WIN32
                if (active) {
                    previous = GetThreadPriority(GetCurrentThread());
                    if (previous != THREAD_PRIORITY_ERROR_RETURN && previous > THREAD_PRIORITY_BELOW_NORMAL) {
                        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    }
                }
#endif
            }
            ~HoldSlot() {
                if (!active) {
                    return;
                }
#ifdef _WIN32
                if (previous != THREAD_PRIORITY_ERROR_RETURN && previous > THREAD_PRIORITY_BELOW_NORMAL) {
                    SetThreadPriority(GetCurrentThread(), previous);
                }
#endif
                {
                    std::scoped_lock lock{cache.async_mutex};
                    --cache.job_queues.hold_running;
                }
                cache.async_cv.notify_all();
                cache.urgent_cv.notify_all();
            }
        } hold_slot{*this, hold_job};
        // sotc: SOTC_BLOCK_LEDGER job queue report: the wait ends here, the run ends when the job is done.
        const u64 job_taken_ns = SotcJobClockNs();
        const u64 job_wait_ns = job_taken_ns - std::min(job_taken_ns, job->created_ns);
        if (SotcTrace::kOn && job->created_ns != 0) {
            // sotc: queue latency on the worker track, distinct from execution (same monotonic clock).
            SotcTrace::Span("pipeline job queued", job_taken_ns - job_wait_ns, job_taken_ns,
                            job->hash, static_cast<u64>(job_class));
        }
        const SotcTrace::Scope job_trace{"pipeline job run", job->hash, static_cast<u64>(job_class)};
        const auto note_job = [&] {
            if (SotcBlockLedger::kOn) {
                NoteJob(job_class, job_wait_ns, SotcJobClockNs() - job_taken_ns);
            }
        };
        if (job->lazy) { // sotc: SOTC_NB_IO pack load
            RunLazyLoad(*job->lazy);
            note_job();
            std::scoped_lock lock{async_mutex};
            async_done.push_back(std::move(job));
            async_done_count.fetch_add(1, std::memory_order_release);
            continue;
        }
        if (job->translate) {
            RunAsyncTranslate(*job->translate);
            note_job();
            std::scoped_lock lock{async_mutex};
            async_done.push_back(std::move(job));
            async_done_count.fetch_add(1, std::memory_order_release);
            // sotc: a dispatch may wait for it (WaitPendingTranslation).
            translate_done_cv.notify_all();
            continue;
        }
        if (job->compute && (job->compute_fast || job->compute_opt)) { // sotc: SOTC_COMPUTE_FAST
            if (job->compute_opt) {
                job->compute->BuildOptimized();
            } else if (job->compute->BuildFast()) {
                QueueComputeOptimized(job->compute);
            }
            note_job();
            continue;
        }
        if (job->compute) {
            const auto start = std::chrono::steady_clock::now();
            job->compute->Build();
            LogSlowBuild("compute", start);
            if (job->addr_table) { // sotc: SOTC_ADDR_FALLBACK: its dispatches take it from now on (IsReady)
                g_addr_fb.pending.fetch_sub(1, std::memory_order_relaxed);
                g_addr_fb.ready.fetch_add(1, std::memory_order_relaxed);
            }
            if (job->pre_compute) { // sotc: SOTC_PRETRANSLATE_COMPUTE: done when the pipeline is built (or refused)
                job->compute->WaitBuilt();
                SotcPretranslate::NoteBuilt(2, job->compute->Handle() != vk::Pipeline{});
                if (Shader::ComputeSpecFree()) { // sotc: SOTC_COMPUTE_SPECFREE ("compile at load")
                    g_specfree.compile_ns.fetch_add(
                        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count()),
                        std::memory_order_relaxed);
                }
                if (job->pack_compute) { // sotc: SOTC_COMPUTE_PACK: the last pack build's time and the worker time
                    const auto now = std::chrono::steady_clock::now();
                    pack_compute_last_ns.store(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                        std::memory_order_relaxed);
                    pack_compute_build_ns.fetch_add(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count(),
                        std::memory_order_relaxed);
                }
                pre_builds_pending.fetch_sub(1, std::memory_order_relaxed);
                // sotc: SOTC_COMPILE_STREAM: the last access of this worker (the hold may destroy it from now on).
                job->compute->sotc_worker_done.store(true, std::memory_order_release);
            }
            if (job->manifest) { // sotc: SOTC_PIPE_MANIFEST: the hold counts it as done (built or refused)
                job->compute->WaitBuilt();
                g_manifest_stats.compute_built.fetch_add(1, std::memory_order_relaxed);
            }
            SotcStats::async_compute_built.fetch_add(1, std::memory_order_relaxed);
            SotcStats::async_compute_build_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                std::memory_order_relaxed);
            note_job();
            continue;
        }
        if (job->lib_parts != 0) {
            RunLibraryJob(*job); // sotc: SOTC_GPL_PREDICT / SOTC_GPL_PARALLEL_LIBS library job
        } else if (job->cancelled.load(std::memory_order_acquire)) {
            // sotc: SOTC_NB_SCHED: the command processor built this pipeline itself; nothing to build.
            g_sched_cancelled.fetch_add(1, std::memory_order_relaxed);
        } else {
            GplLibraryCache::SetLibrariesOnly(job->manifest_libs); // sotc: SOTC_GPL_NODROP boot libraries, no link
            if (job->manifest_libs) {
                GplLibraryCache::SetOrigin(GplLibraryCache::OriginManifest); // sotc: SOTC_GPL_PREDICT: never evicted
            }
            const auto graphics_start = std::chrono::steady_clock::now();
            job->pipeline = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, layout_cache, StagesProfile(job->infos), job->key, *pipeline_cache,
                job->infos,
                job->runtime_infos, job->fetch_shader ? &*job->fetch_shader : nullptr, job->modules,
                job->sdata, job->preloaded, true, job->link_only ? &gpl_libs : nullptr);
            GplLibraryCache::SetLibrariesOnly(false);
            GplLibraryCache::SetOrigin(GplLibraryCache::OriginDefault);
            LogSlowBuild("graphics", graphics_start);
            SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
        }
        note_job();
        std::scoped_lock lock{async_mutex};
        const bool boot = job->preloaded; // sotc: N5
        async_done.push_back(std::move(job));
        async_done_count.fetch_add(1, std::memory_order_release);
        if (boot) {
            N5NoteBootBuilt();
        }
        g_preload_cv.notify_all();
    }
}

bool PipelineCache::TryFastLink(std::unique_ptr<GraphicsPipeline>& slot, bool allow_build, bool probe) {
    const N5Clock::time_point link_start = N5Clock::now(); // sotc: N5
    if (!GraphicsPipeline::FastLinkable(graphics_key, infos, runtime_infos, StagesProfile(infos))) {
        g_gpl_counters.unlinkable.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    GraphicsPipeline::SerializationSupport sdata{};
    // sotc: SOTC_NB_GPL: the command processor only links libraries that exist; a missing one is built by a worker
    // (QueueLinkJob).
    // allow_build: a tiny full-screen draw (never skipped) builds its missing libraries here, ~17 ms each, instead of
    // a whole pipeline.
    // sotc: SOTC_GPL_PARALLEL_LIBS: a probe links only what exists (GplLibraryCache::Missing tells what does not).
    const bool lookup_only = probe || (SotcNonBlocking::Gpl() && !allow_build);
    GplLibraryCache::SetLookupOnly(lookup_only);
    auto pipe = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, layout_cache, StagesProfile(infos), graphics_key, *pipeline_cache,
        infos, runtime_infos, fetch_shader, modules, sdata, false, false, &gpl_libs);
    GplLibraryCache::SetLookupOnly(false);
    if (!pipe->IsLinked()) {
        return false;
    }
    N5NoteSyncCompile(N5Kind::Link, link_start); // sotc: N5
    slot = std::move(pipe);
    return true;
}

// sotc: SOTC_GPL_NODROP. A linked pipeline is only taken when its optimized build can follow on a worker (else the
// linked one would serve its draws for good): workers, a pipeline built from no draw state, no descriptor heap (heap
// pipelines are never linked, the constructor would build the full pipeline), and a state the libraries cover.
bool PipelineCache::NoDropLinkable() {
    return SotcNonBlocking::GplNoDrop() && instance.IsGraphicsPipelineLibrary() && !async_workers.empty() &&
           !instance.IsDescriptorHeapEnabled() && AsyncPipelineEligible() &&
           GraphicsPipeline::FastLinkable(graphics_key, infos, runtime_infos, StagesProfile(infos));
}

// sotc: SOTC_GPL_NODROP. TryFastLink with the missing libraries built here (a library is far cheaper than the full
// pipeline; the manifest's boot hold built those of every known stage), counted and timed apart from the link.
bool PipelineCache::NoDropLink(std::unique_ptr<GraphicsPipeline>& slot) {
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::GplLink,
                                       std::hash<GraphicsPipelineKey>{}(graphics_key)}; // sotc: inclusive
    const auto start = std::chrono::steady_clock::now();
    const u64 libs_before = GplLibraryCache::ThreadBuilt();
    const u64 link_before = g_gpl_counters.link_ns.load(std::memory_order_relaxed);
    GplLibraryCache::SetDrawUse(true); // sotc: SOTC_GPL_PREDICT: predicted libraries found here count as used
    bool ok = false;
    // sotc: SOTC_GPL_PARALLEL_LIBS: a link of the libraries that exist first; when the vertex and the pixel library
    // are both missing, two urgent jobs build them at once (counted apart: parallel waits), and the link below finds
    // them. Any other missing library is built here, as before.
    u64 par_ns = 0, par_libs = 0;
    if (SotcNonBlocking::GplParallelLibs()) {
        GplLibraryCache::ResetMissing();
        ok = TryFastLink(slot, false, true);
        if (!ok) {
            const auto par_start = std::chrono::steady_clock::now();
            const u64 par_before = GplLibraryCache::ThreadBuilt();
            if (BuildLibrariesParallel(GplLibraryCache::Missing())) {
                par_libs = GplLibraryCache::ThreadBuilt() - par_before;
                par_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - par_start)
                                              .count());
            }
        }
    }
    if (!ok) {
        ok = TryFastLink(slot, true);
    }
    GplLibraryCache::SetDrawUse(false);
    const u64 libs = GplLibraryCache::ThreadBuilt() - libs_before - par_libs;
    if (par_ns != 0 || libs != 0) {
        // sotc: plan step 3: the command processor waited for the driver to build this draw's libraries (parallel
        // jobs or its own builds). Counted always, SOTC_FPS_LEAN included, in "cp blocked" and "cp libwait".
        const u64 wait_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        SotcStats::cp_libwait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
        SotcStats::cp_blocked_ns.fetch_add(wait_ns, std::memory_order_relaxed);
    }
    if (libs != 0) {
        const u64 elapsed_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        const u64 total_ns = elapsed_ns > par_ns ? elapsed_ns - par_ns : 0;
        const u64 link_ns = g_gpl_counters.link_ns.load(std::memory_order_relaxed) - link_before;
        g_nodrop.sync_libs.fetch_add(libs, std::memory_order_relaxed);
        g_nodrop.sync_lib_ns.fetch_add(total_ns > link_ns ? total_ns - link_ns : 0, std::memory_order_relaxed);
        g_nodrop.sync_lib_draws.fetch_add(1, std::memory_order_relaxed);
    }
    if (ok) {
        g_nodrop.links.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_nodrop.link_refused.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

void PipelineCache::CollectAsyncPipelines() {
    if (SotcBlockLedger::kOn) {
        ReportJobs(); // sotc: SOTC_BLOCK_LEDGER job queue report, every 5 s
    }
    std::vector<std::unique_ptr<AsyncPipelineJob>> done;
    {
        std::scoped_lock lock{async_mutex};
        done.swap(async_done);
        async_done_count.store(0, std::memory_order_relaxed);
    }
    for (auto& job : done) {
        if (job->translate) {
            InstallTranslation(*job->translate);
            continue;
        }
        if (job->lazy) { // sotc: SOTC_NB_IO pack load
            InstallLazyLoad(*job->lazy);
            continue;
        }
        if (job->lib_parts != 0) { // sotc: SOTC_GPL_PREDICT / SOTC_GPL_PARALLEL_LIBS: only its libraries mattered
            if (lib_predict && job->lib_origin == GplLibraryCache::OriginPredicted) {
                lib_predict->inflight.erase(job->pack_id); // sotc: evictable from now on
            }
            continue;
        }
        if (job->addr_table && !job->compute) { // sotc: SOTC_ADDR_FALLBACK: installed below for the next draw
            g_addr_fb.pending.fetch_sub(1, std::memory_order_relaxed);
            g_addr_fb.ready.fetch_add(1, std::memory_order_relaxed);
        }
        if (job->manifest && !job->compute && manifest) { // sotc: SOTC_PIPE_MANIFEST: installed below, as a draw's
            if (manifest->graphics_building > 0) {
                --manifest->graphics_building;
            }
            if (job->pipeline && job->pipeline->Handle()) {
                ++manifest->built_graphics;
            } else {
                ++manifest->refused;
            }
        }
        if (pipe_predict && !job->compute) { // sotc: SOTC_PIPE_PREDICT throttle
            pipe_predict->inflight.erase(job->key);
        }
        if (!job->compute && !queued_graphics.empty()) { // sotc: SOTC_NB_SCHED
            if (const auto f = queued_graphics.find(job->key); f != queued_graphics.end() && f->second == job.get()) {
                queued_graphics.erase(f);
            }
        }
        const auto it = graphics_pipelines.find(job->key);
        if (job->manifest_libs) { // sotc: SOTC_GPL_NODROP: the libraries of a manifest pipeline (no pipeline)
            if (manifest && manifest->libs_building > 0) {
                --manifest->libs_building;
            }
            g_nodrop.boot_libs_done.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (job->warm_lib) { // sotc: SOTC_NB_GPL (P4): only its library mattered; the linked pipeline was never bound
            ++warm_built;
            continue;
        }
        if (job->link_only) { // sotc: SOTC_NB_GPL link job: serves the draws until the optimized build
            link_pending.erase(job->key);
            if (it != graphics_pipelines.end() && !it.value() && job->pipeline && job->pipeline->Handle()) {
                it.value() = std::move(job->pipeline);
                last_graphics_pipeline = nullptr;
            }
            continue;
        }
        if (it == graphics_pipelines.end()) {
            continue;
        }
        if (it.value()) {
            // sotc: SOTC_PIPE_GPL: the optimized build replaces the fast-linked pipeline the draws
            // were using. The linked one is retired, not destroyed (RetireLinkedPipeline): back-thread
            // ops may still hold it.
            // sotc: SOTC_GPL_NODROP: a boot preload's build replaces it too (a draw linked while the preload was
            // queued; it was kept for good before). A preloaded pipeline is in the cache already: not registered.
            if (job->pipeline && job->pipeline->Handle() && it.value()->IsLinked()) {
                auto linked = std::move(it.value());
                it.value() = std::move(job->pipeline);
                last_graphics_pipeline = nullptr; // the memo may point at the replaced one
                if (job->preloaded) {
                    it.value()->use_count = job->uses + linked->use_count; // sotc: N5
                    it.value()->saved_use_count = job->uses;
                } else {
                    RegisterPipelineData(job->key, job->hash, job->sdata);
                    ++num_new_pipelines;
                }
                g_gpl_counters.replaced.fetch_add(1, std::memory_order_relaxed);
                RetireLinkedPipeline(std::move(linked));
                continue;
            }
            // sotc: SOTC_GPL_NODROP: a preload the driver refused, under a linked pipeline: built again as a draw's
            // pipeline (as the next draw of an erased entry would), so the linked one is still replaced.
            if (job->preloaded && it.value()->IsLinked()) {
                job->preloaded = false;
                job->pipeline.reset();
                {
                    std::scoped_lock lock{async_mutex};
                    PushJobLocked(JobLane::Graphics, std::move(job));
                }
                async_cv.notify_one();
                continue;
            }
            // A full-screen pass built it on the command processor meanwhile (that one may be
            // in use): the worker's copy is dropped.
            if (!job->preloaded) {
                RegisterPipelineData(job->key, job->hash, job->sdata);
                ++num_new_pipelines;
            }
            continue;
        }
        if (job->preloaded) {
            // Already in the cache. One the driver refused is dropped: the next draw that needs
            // it compiles it, as when WarmUp builds it itself and it fails.
            if (job->pipeline->Handle()) {
                it.value() = std::move(job->pipeline);
                it.value()->use_count = job->uses; // sotc: N5
                it.value()->saved_use_count = job->uses;
            } else {
                graphics_pipelines.erase(it);
            }
            continue;
        }
        it.value() = std::move(job->pipeline);
        RegisterPipelineData(job->key, job->hash, job->sdata);
        ++num_new_pipelines;
    }
    ReleaseBuiltComputeModules(false); // sotc: SOTC_COMPUTE_RELEASE, at most once per second (no-op when empty)
}

// sotc: N5. The graphics pipelines of the boot prebuild are built by the workers in the order WarmUp
// queued them (most used first). This waits for them on the loading screen, up to the cap, and
// installs each as it is built. Past the cap it returns: the rest is collected by the draws as before.
void PipelineCache::WaitPreloadBoot(N5Clock::time_point start) {
    static const double cap_s = [] {
        const char* v = std::getenv("SOTC_N5_PSO_WAIT_S");
        const double s = v ? std::atof(v) : 15.0;
        return s < 0.0 ? 0.0 : s;
    }();
    const auto deadline =
        start + std::chrono::duration_cast<N5Clock::duration>(std::chrono::duration<double>(cap_s));
    g_n5.boot_start_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count(),
        std::memory_order_relaxed);
    g_n5.boot_closed.store(true, std::memory_order_relaxed);
    bool capped = false;
    for (;;) {
        CollectAsyncPipelines();
        const u64 total = g_n5.boot_total.load(std::memory_order_relaxed);
        const u64 built = g_n5.boot_built.load(std::memory_order_relaxed);
        SotcStats::warmup_total.store(static_cast<u32>(total), std::memory_order_relaxed);
        SotcStats::warmup_done.store(static_cast<u32>(built), std::memory_order_relaxed);
        if (built >= total && async_done_count.load(std::memory_order_acquire) == 0) {
            break;
        }
        std::unique_lock lock{async_mutex};
        if (!g_preload_cv.wait_until(lock, deadline, [this] {
                return async_done_count.load(std::memory_order_acquire) != 0 ||
                       g_n5.boot_built.load(std::memory_order_relaxed) >=
                           g_n5.boot_total.load(std::memory_order_relaxed);
            })) {
            capped = true;
            break;
        }
    }
    const u64 total = g_n5.boot_total.load(std::memory_order_relaxed);
    const u64 built = g_n5.boot_built.load(std::memory_order_relaxed);
    if (built >= total) {
        g_n5.boot_end_ns.store(N5NowNs(), std::memory_order_relaxed);
    }
    std::fprintf(stderr,
                 "SOTC N5 PSO boot: %llu of %llu graphics pipelines built in %.0f ms%s\n",
                 static_cast<unsigned long long>(built), static_cast<unsigned long long>(total),
                 static_cast<double>(N5NowNs() - g_n5.boot_start_ns.load()) / 1e6,
                 capped ? " (cap reached, the rest builds in the background)" : "");
}

// sotc: N5. Rewrites the use count of each pipeline blob whose count changed this session. Only with
// N5 on; a process that is killed keeps the counts of its last clean exit (as the driver cache).
void PipelineCache::SaveUseCounts() {
    if (!N5PsoEnabled() || !Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    const auto start = N5Clock::now();
    u32 written = 0;
    for (auto& [key, pipeline] : graphics_pipelines) {
        if (!pipeline || pipeline->use_count == pipeline->saved_use_count) {
            continue;
        }
        const auto name = fmt::format("g_{:#018x}", std::hash<GraphicsPipelineKey>{}(key));
        if (RewritePipelineUseCount(name, pipeline->use_count)) {
            pipeline->saved_use_count = pipeline->use_count;
            ++written;
        }
    }
    for (auto& [key, pipeline] : compute_pipelines) {
        if (!pipeline || pipeline->use_count == pipeline->saved_use_count) {
            continue;
        }
        const auto name = fmt::format("c_{:#018x}", key.value);
        if (RewritePipelineUseCount(name, pipeline->use_count)) {
            pipeline->saved_use_count = pipeline->use_count;
            ++written;
        }
    }
    std::fprintf(stderr, "SOTC N5 PSO: use counts saved for %u pipelines in %.0f ms\n", written,
                 std::chrono::duration<double, std::milli>(N5Clock::now() - start).count());
}

void PipelineCache::QueueAsyncTranslate(HwStage hw_stage, SwStage sw_stage,
                                        const Shader::ShaderParams& params,
                                        const Shader::RuntimeInfo& rt,
                                        const Shader::Backend::Bindings& start, Program* program,
                                        const Shader::StageSpecialization* spec, size_t perm_idx) {
    auto job = std::make_unique<AsyncPipelineJob>();
    auto& t = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
    t.hw_stage = hw_stage;
    t.sw_stage = sw_stage;
    t.hash = params.hash;
    t.variant = params.variant; // sotc: SOTC_HEAP
    t.guest_hash = params.guest_hash;
    // The user data registers change with the next draws: the worker reads a copy. The code
    // stays in guest memory (inline constants are addressed from it).
    std::ranges::copy(params.user_data, t.user_data.begin());
    t.code = params.code;
    t.runtime_info = rt;
    t.start = start;
    t.program = program;
    t.perm_idx = perm_idx;
    if (spec) {
        t.spec = *spec;
    }
    {
        std::scoped_lock lock{async_mutex};
        PushJobLocked(JobLane::Translate, std::move(job));
    }
    async_cv.notify_one();
}

bool PipelineCache::PrefetchComputeProgram(const AmdGpu::ComputeProgram& cs) {
    if (!compute_prefetch || EmulatorSettings.IsShaderCollect()) {
        return false;
    }
    // The registers come from a scan ahead of the dispatches: only a mapped code address is
    // read, and a failed binary info search gives up instead of stopping the port.
    const auto* code = cs.Address<const u32*>();
    if (!code || !Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(code))) {
        return false;
    }
    std::optional<Shader::ShaderParams> params;
    const bool throws = Common::g_assert_throws;
    Common::g_assert_throws = true;
    try {
        params.emplace(AmdGpu::GetParams(cs));
    } catch (const Common::AssertFailure&) {
    }
    Common::g_assert_throws = throws;
    if (!params || params->code.empty() || IsProgramKnown(params->hash)) {
        return false;
    }
    auto rt = runtime_infos[static_cast<u32>(SwStage::Compute)]; // not default-constructible
    rt.Initialize(HwStage::Compute, SwStage::Compute);
    FillComputeRuntimeInfo(rt, cs);
    pending_programs.insert(params->hash);
    QueueAsyncTranslate(HwStage::Compute, SwStage::Compute, *params, rt,
                        Shader::Backend::Bindings{}, nullptr, nullptr, 0);
    SotcStats::compute_prefetch_queued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PipelineCache::WaitPendingTranslation(u64 hash) {
    const auto start = std::chrono::steady_clock::now();
    {
        // No worker started it yet: translating it here is quicker than waiting behind the
        // jobs ahead of it. Translation jobs sit right after the compute jobs.
        std::scoped_lock lock{async_mutex};
        auto& lane = job_queues[JobLane::Translate];
        const auto it = std::find_if(lane.begin(), lane.end(), [hash](const auto& job) {
            // sotc: never a SOTC_PIPE_MANIFEST translation (the manifest waits for its own jobs).
            return !job->translate->program && job->translate->hash == hash && job->translate->manifest_stage < 0;
        });
        if (it != lane.end()) {
            lane.erase(it);
            pending_programs.erase(hash);
            SotcStats::compute_prefetch_taken_back.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // A worker is translating it: it finishes no later than a second translation here would.
    // The deadline only guards against a worker that never finishes; past it the shader is
    // translated here and the worker's copy is dropped when installed (InstallTranslation).
    const auto deadline = start + std::chrono::seconds{1};
    for (;;) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            last_graphics_pipeline = nullptr; // a refused preloaded pipeline may be erased
            CollectAsyncPipelines();
        }
        if (!pending_programs.contains(hash)) {
            break;
        }
        std::unique_lock lock{async_mutex};
        if (!translate_done_cv.wait_until(lock, deadline, [this] {
                return async_done_count.load(std::memory_order_relaxed) != 0;
            })) {
            SotcStats::compute_prefetch_timeouts.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    SotcStats::compute_prefetch_waits.fetch_add(1, std::memory_order_relaxed);
    SotcStats::compute_prefetch_wait_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count(),
        std::memory_order_relaxed);
}

// sotc: SOTC_SHADER_ADDR (table mode). The compute lookup may not return without a module (RefreshComputeKey passes
// async_ok false), so a program or permutation it lacks was translated inline: TranslateProgram, EmitSPIRV and the
// driver's module compile on the command processor (the cold table-mode runs showed the CP inside nvgpucomp64.dll).
// The translation goes to a worker on the Urgent lane instead and the CP waits for it, installing finished jobs as
// WaitPendingTranslation does. Descriptor mode never calls this (byte-identical).
bool PipelineCache::TranslateComputeOnWorker(const Shader::ShaderParams& params, const Shader::RuntimeInfo& rt,
                                             const Shader::Backend::Bindings& start, Program* program,
                                             const Shader::StageSpecialization* spec, size_t perm_idx) {
    if (profile.shader_addr == 0 || async_workers.empty() || EmulatorSettings.IsShaderCollect() ||
        (program && !spec)) {
        return false;
    }
    const auto begin = std::chrono::steady_clock::now();
    auto job = std::make_unique<AsyncPipelineJob>();
    auto& t = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
    t.hw_stage = HwStage::Compute;
    t.sw_stage = SwStage::Compute;
    t.hash = params.hash;
    t.variant = params.variant;
    t.guest_hash = params.guest_hash;
    std::ranges::copy(params.user_data, t.user_data.begin()); // the worker reads a copy (as QueueAsyncTranslate)
    t.code = params.code;
    t.runtime_info = rt;
    t.start = start;
    t.program = program;
    t.perm_idx = perm_idx;
    if (program) {
        t.spec = *spec;
        program->pending_specs.push_back(*spec); // InstallTranslation removes it (installed, stale or failed)
    } else {
        pending_programs.insert(params.hash); // InstallTranslation erases it (installed or failed)
    }
    {
        std::scoped_lock lock{async_mutex};
        PushJobLocked(JobLane::Urgent, std::move(job));
    }
    async_cv.notify_all();
    const auto done = [&] {
        return program ? std::ranges::find(program->pending_specs, *spec) == program->pending_specs.end()
                       : !pending_programs.contains(params.hash);
    };
    // The deadline only guards against a worker that never finishes; past it the CP translates inline as before (the
    // worker's copy is dropped when installed).
    const auto deadline = begin + std::chrono::seconds{30};
    bool timed_out = false;
    for (;;) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            last_graphics_pipeline = nullptr; // a refused preloaded pipeline may be erased
            pipe_fast_gfx.valid = false;
            CollectAsyncPipelines();
        }
        if (done()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        std::unique_lock lock{async_mutex};
        translate_done_cv.wait_for(lock, std::chrono::milliseconds{20}, [this] {
            return async_done_count.load(std::memory_order_relaxed) != 0;
        });
    }
    g_specfree.cp_worker.fetch_add(1, std::memory_order_relaxed);
    g_specfree.cp_worker_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                           std::chrono::steady_clock::now() - begin)
                                                           .count()),
                                      std::memory_order_relaxed);
    if (timed_out) {
        g_specfree.cp_worker_timeouts.fetch_add(1, std::memory_order_relaxed);
        if (program) {
            // The inline translation below adds the permutation; the worker's copy then finds it there.
            if (const auto it = std::ranges::find(program->pending_specs, *spec); it != program->pending_specs.end()) {
                program->pending_specs.erase(it);
            }
        }
        return false;
    }
    return true;
}

void PipelineCache::RunAsyncTranslate(AsyncPipelineJob::Translate& t) {
    static thread_local Shader::Pools worker_pools;
    const auto start_time = std::chrono::steady_clock::now();
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{t.user_data},
        .code = t.code,
        .hash = t.hash,
        .variant = t.variant, // sotc: SOTC_HEAP
        .guest_hash = t.guest_hash,
    };
    auto binding = t.start;
    auto runtime_info = t.runtime_info;
    // The tables the user data points to may already hold the game's next data: a translation
    // that asserts on what it read gives the shader back to the command processor.
    Common::g_assert_throws = true;
    try {
        TranslateOnWorker(t, params, binding, runtime_info, worker_pools);
    } catch (const Common::AssertFailure&) {
        t.failed = true;
        t.new_program.reset();
        if (t.module) {
            instance.GetDevice().destroyShaderModule(t.module);
            t.module = vk::ShaderModule{};
        }
    }
    Common::g_assert_throws = false;
    SotcStats::async_translate_built.fetch_add(1, std::memory_order_relaxed);
    SotcStats::async_translate_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start_time)
            .count(),
        std::memory_order_relaxed);
    if (Shader::ComputeSpecFree() && t.pretranslate && !t.program && t.sw_stage == SwStage::Compute) {
        // sotc: SOTC_COMPUTE_SPECFREE: the worker time of a compute prebuild's translation ("compile at load")
        g_specfree.compile_ns.fetch_add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start_time)
                                 .count()),
            std::memory_order_relaxed);
    }
}

void PipelineCache::TranslateOnWorker(AsyncPipelineJob::Translate& t,
                                      const Shader::ShaderParams& params,
                                      Shader::Backend::Bindings& binding,
                                      Shader::RuntimeInfo& runtime_info,
                                      Shader::Pools& worker_pools) {
    if (!t.program) {
        // sotc: SOTC_PIPE_MANIFEST: a manifest stage is translated with its draw's sharp fields (hint), its draw's
        // fetch shader data, and user data pointing at a table of its fetch attributes' V#.
        const bool manifest_job = t.manifest_stage >= 0;
        if (manifest_job) {
            ManifestUserData(t);
        }
        struct FetchScope {
            explicit FetchScope(const Shader::Gcn::FetchShaderData* data) {
                Shader::Gcn::SetSyntheticFetchShader(data);
            }
            ~FetchScope() {
                Shader::Gcn::SetSyntheticFetchShader(nullptr);
            }
        } fetch_scope{manifest_job && t.manifest_fetch ? &*t.manifest_fetch : nullptr};
        // sotc: SOTC_PRETRANSLATE_COMPUTE_HINTS: a compute prebuild takes the fields its program's last miss recorded,
        // looked up by the game's hash (a variant's job carries it in guest_hash).
        SotcPretranslate::HintPtr compute_hint;
        if (!manifest_job && t.pretranslate && t.sw_stage == SwStage::Compute) {
            compute_hint = SotcPretranslate::AcquireHint(t.guest_hash != 0 ? t.guest_hash : t.hash);
            t.hinted = compute_hint != nullptr; // sotc: SOTC_COMPUTE_PACK report (hints / synthetic)
        }
        SotcPretranslate::HintScope hint_scope{manifest_job ? t.manifest_hint : compute_hint};
        t.new_program = std::make_unique<Program>(t.hw_stage, t.sw_stage, params);
        t.new_program->info.sotc_synthetic = t.pretranslate; // sotc: SOTC_PRETRANSLATE
        t.new_program->info.sotc_synthetic_variant = t.synthetic_variant;
        t.new_program->info.sotc_synthetic_mask = t.synthetic_mask; // sotc: SOTC_COMPUTE_SPECFREE
        t.module = CompileModule(t.new_program->info, runtime_info, params.code, 0, binding,
                                 &worker_pools, &t.spv);
        t.spec.emplace(t.new_program->info, runtime_info, profile, t.start);
        t.new_program->info.sotc_synthetic = false;
    } else {
        auto new_info = Shader::Info(t.hw_stage, t.sw_stage, params);
        t.module = CompileModule(new_info, runtime_info, params.code, t.perm_idx, binding,
                                 &worker_pools, &t.spv);
        t.bindless_answers = new_info.bindless; // sotc: merged into the program when installed
        // The command processor made spec from the tables of its draw; the module was made from
        // what the tables hold now. Different: installing it under spec would bind wrong code.
        const Shader::StageSpecialization worker_spec{new_info, t.runtime_info, profile,
                                                      t.start};
        t.stale = !(worker_spec == *t.spec);
    }
}

void PipelineCache::InstallTranslation(AsyncPipelineJob::Translate& t) {
    if (t.manifest_stage >= 0) { // sotc: SOTC_PIPE_MANIFEST: installed by the next pump, outside any lookup
        if (manifest) {
            manifest->finished.push_back(std::make_unique<AsyncPipelineJob::Translate>(std::move(t)));
        } else if (t.module) {
            instance.GetDevice().destroyShaderModule(t.module);
        }
        return;
    }
    if (!t.program && t.pretranslate) {
        PreJobDone(t.hash); // sotc: SOTC_PRETRANSLATE_STREAM / SOTC_COMPILE_STREAM
    }
    if (!t.program && t.pretranslate && t.sw_stage == SwStage::Compute) {
        InstallComputePrebuild(t); // sotc: SOTC_PRETRANSLATE_COMPUTE
        return;
    }
    if (!t.program && t.pretranslate) {
        // sotc: SOTC_PRETRANSLATE. A pre-translation that failed says nothing about the draws (their tables were
        // never read): the program stays unknown, and its first draw translates it as before.
        if (pre_in_flight > 0) {
            --pre_in_flight;
        }
        struct FeedNext {
            PipelineCache& cache;
            ~FeedNext() {
                cache.FeedPretranslate();
            }
        } feed_next{*this};
        const int stage = t.sw_stage == SwStage::Vertex ? 1 : 0;
        SotcPretranslate::NoteBuilt(stage, !t.failed);
        if (!t.failed && t.first_variant && t.synthetic_variant == 0 && t.new_program &&
            t.new_program->info.sotc_synthetic_reads) {
            // The program reads images whose texel type the code does not show: the other verdict as one more
            // variant, queued behind every other job.
            auto job = std::make_unique<AsyncPipelineJob>();
            auto& v = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
            v.hw_stage = t.hw_stage;
            v.sw_stage = t.sw_stage;
            v.hash = t.hash;
            v.owned_code = t.owned_code;
            v.code = v.owned_code;
            v.runtime_info = t.runtime_info;
            v.start = t.start;
            v.pretranslate = true;
            v.synthetic_variant = 1;
            SotcPretranslate::NoteQueued(stage);
            PreJobQueued(t.hash); // sotc: SOTC_PRETRANSLATE_STREAM
            pre_backlog.push_back(std::move(job));
        }
        if (pre_pending.erase(t.hash) != 0) {
            pending_programs.erase(t.hash);
        }
        if (!t.failed && program_cache.contains(t.hash)) {
            // Another variant of this program (another layout, another draw kind), or a program a draw translated
            // meanwhile: the variant is one more permutation unless that key is there already.
            auto& program = *program_cache.find(t.hash).value();
            if (!SameBindingLayout(program.info, t.new_program->info)) {
                // sotc: its descriptors would be written for the program's Info (see SameBindingLayout): dropped.
                static std::atomic<u32> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                    std::fprintf(stderr,
                                 "SOTC PRETRANSLATE: variant of %#llx dropped, its resources differ from the "
                                 "program's (buffers %zu/%zu, images %zu/%zu, samplers %zu/%zu)\n",
                                 static_cast<unsigned long long>(t.hash), t.new_program->info.buffers.size(),
                                 program.info.buffers.size(), t.new_program->info.images.size(),
                                 program.info.images.size(), t.new_program->info.samplers.size(),
                                 program.info.samplers.size());
                }
                if (t.module) {
                    instance.GetDevice().destroyShaderModule(t.module);
                }
                return;
            }
            if (std::ranges::find(program.modules, *t.spec, &Program::Module::spec) == program.modules.end() &&
                std::ranges::find(program.pending_specs, *t.spec) == program.pending_specs.end()) {
                const size_t perm_idx = program.modules.size();
                Shader::CombineBindlessCache(program.info.bindless, t.new_program->info.bindless);
                if (!((pre_holding || SotcPretranslate::Precompile()) &&
                      PrePackAdd(t.hash, static_cast<u32>(perm_idx),
                                 SerializeShaderMeta(program.info, *t.spec, HashCombine(t.hash, perm_idx), perm_idx),
                                 t.spv))) {
                    RegisterShaderBinary(std::move(t.spv), t.hash, perm_idx);
                    SotcPredict::SetPretranslating(true);
                    RegisterShaderMeta(program.info, t.spec->fetch_shader_data, *t.spec,
                                       HashCombine(t.hash, perm_idx), perm_idx);
                    SotcPredict::SetPretranslating(false);
                }
                program.AddPermut(t.module, std::move(*t.spec));
                return;
            }
        }
        if (t.failed || program_cache.contains(t.hash)) {
            if (t.module) {
                instance.GetDevice().destroyShaderModule(t.module);
            }
            return;
        }
        auto [it, is_new] = program_cache.try_emplace(t.hash);
        it.value() = std::move(t.new_program);
        auto& program = it.value();
        program->sotc_pre = true;
        if (!((pre_holding || SotcPretranslate::Precompile()) &&
              PrePackAdd(t.hash, 0, SerializeShaderMeta(program->info, *t.spec, HashCombine(t.hash, 0), 0), t.spv))) {
            RegisterShaderBinary(std::move(t.spv), t.hash, 0);
            SotcPredict::SetPretranslating(true);
            RegisterShaderMeta(program->info, t.spec->fetch_shader_data, *t.spec, HashCombine(t.hash, 0), 0);
            SotcPredict::SetPretranslating(false);
        }
        program->AddPermut(t.module, std::move(*t.spec));
        program->info.UserData() = {};
        return;
    }
    if (!t.program) {
        pending_programs.erase(t.hash);
        if (t.failed) {
            SotcStats::async_translate_stale.fetch_add(1, std::memory_order_relaxed);
            in_place_programs.insert(t.hash);
            return;
        }
        auto [it, is_new] = program_cache.try_emplace(t.hash);
        if (!is_new) {
            // Translated meanwhile on the command processor (by a draw that may not wait).
            instance.GetDevice().destroyShaderModule(t.module);
            return;
        }
        it.value() = std::move(t.new_program);
        auto& program = it.value();
        RegisterShaderBinary(std::move(t.spv), t.hash, 0);
        RegisterShaderMeta(program->info, t.spec->fetch_shader_data, *t.spec,
                           HashCombine(t.hash, 0), 0);
        program->AddPermut(t.module, std::move(*t.spec));
        // The user data seen by the worker goes away with the job; every lookup sets its own.
        program->info.UserData() = {};
        return;
    }
    auto& program = *t.program;
    if (const auto pending = std::ranges::find(program.pending_specs, *t.spec);
        pending != program.pending_specs.end()) {
        program.pending_specs.erase(pending);
    }
    if (t.stale || t.failed) {
        SotcStats::async_translate_stale.fetch_add(1, std::memory_order_relaxed);
        program.translate_in_place = true;
        instance.GetDevice().destroyShaderModule(t.module);
        return;
    }
    if (std::ranges::find(program.modules, *t.spec, &Program::Module::spec) !=
        program.modules.end()) {
        instance.GetDevice().destroyShaderModule(t.module);
        return;
    }
    const size_t perm_idx = program.modules.size();
    RegisterShaderBinary(std::move(t.spv), t.hash, perm_idx);
    RegisterShaderMeta(program.info, t.spec->fetch_shader_data, *t.spec,
                       HashCombine(t.hash, perm_idx), perm_idx);
    // sotc: the new permutation's bindless answers must agree with the program's (CombineBindlessCache).
    Shader::CombineBindlessCache(program.info.bindless, t.bindless_answers);
    program.AddPermut(t.module, std::move(*t.spec));
}

// sotc: SOTC_PIPE_MEMO. Unset: the graphics key memo of GetGraphicsPipeline is on, as before, and
// the stage and compute memos are off. =0: the key memo is off too. =1: the stage memo (GetProgram)
// and the compute memo (GetComputePipeline) are on. =2: the same, and every memo answer is also
// computed by the slow path; a different answer is counted and the slow result is used.
static bool PipeMemoKeyOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PIPE_MEMO");
        return !v || v[0] != '0';
    }();
    return on;
}

static int PipeMemoStageMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_PIPE_MEMO");
        return v && v[0] == '1' ? 1 : v && v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

namespace {
// sotc: counters of SOTC_PIPE_MEMO, printed by ReportPipeMemo.
// The fields are in the order of kPipeMemoCounters (ReportPipeMemo).
struct PipeMemoStats {
    std::atomic<u64> graphics_lookups{}; ///< graphics lookups (mode 1/2)
    std::atomic<u64> graphics_hits{};    ///< of those, answered by the last-pipeline memo
    std::atomic<u64> stage_lookups{};    ///< stage lookups of an existing program (mode 1/2)
    std::atomic<u64> stage_hits{};       ///< of those, answered by the stage memo
    std::atomic<u64> stage_evictions{};  ///< stage stores that replaced an entry of another key
    std::atomic<u64> stage_foreign{};    ///< GetProgram calls from a thread other than the first
    std::atomic<u64> compute_lookups{};  ///< compute lookups (mode 1/2)
    std::atomic<u64> compute_hits{};     ///< of those, answered by the last-pipeline memo
    std::atomic<u64> mismatch_graphics{}; ///< =2: graphics memo answer differed from the slow path
    std::atomic<u64> mismatch_stage_perm{};     ///< =2: stage memo permutation index differed
    std::atomic<u64> mismatch_stage_module{};   ///< =2: same permutation, other shader module
    std::atomic<u64> mismatch_stage_fetch{};    ///< =2: vertex fetch shader data differed
    std::atomic<u64> mismatch_stage_bindings{}; ///< =2: bindings after the stage differed
    std::atomic<u64> mismatch_stage_pending{};  ///< =2: memo hit, the slow path queued a translation
    std::atomic<u64> mismatch_compute{};  ///< =2: compute memo answer differed from the slow path
};
PipeMemoStats g_pipe_memo;
} // namespace

// sotc: a thread other than the first caller of GetProgram (in a memo mode) takes the slow path.
// Command processor thread only in practice; the check is one atomic load on a hit of the first
// caller and a compare-exchange on the very first call.
bool PipelineCache::PipeMemoOwnerThread() {
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id owner{};
    if (pipe_memo_owner.compare_exchange_strong(owner, self, std::memory_order_acq_rel)) {
        return true;
    }
    if (owner == self) {
        return true;
    }
    g_pipe_memo.stage_foreign.fetch_add(1, std::memory_order_relaxed);
    return false;
}

size_t PipelineCache::StageMemoSet(u64 hash, Shader::HwStage hw_stage, Shader::SwStage sw_stage) {
    const u64 mix = hash ^ (u64(static_cast<u32>(hw_stage)) << 3) ^
                    (u64(static_cast<u32>(sw_stage)) << 9);
    return static_cast<size_t>((mix ^ (mix >> 29)) % kStageMemoSets);
}

// sotc: true when every input the specialization of this stage reads is the one stored in memo.
// The flattened buffer and the user data are compared as they are now (RefreshFlatBuf already ran);
// the tessellation constant buffer is read from guest memory again, as the specialization does.
// A vertex fetch shader: its code words are compared with the copy stored (read through the
// backing view, like the shader binary info, so a draw does not read-fault the guest page).
bool PipelineCache::StageMemoHit(const StageMemo& memo, const Program& program,
                                 Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                                 const Shader::ShaderParams& params,
                                 const Shader::RuntimeInfo& rt,
                                 const Shader::Backend::Bindings& start,
                                 const Shader::TessellationDataConstantBuffer& tess,
                                 bool has_tess, u64 generation) const {
    if (!memo.valid || memo.program != &program || memo.hw_stage != hw_stage ||
        memo.sw_stage != sw_stage || memo.hash != params.hash ||
        memo.modules_size != program.modules.size() || memo.perm >= program.modules.size() ||
        memo.generation != generation || memo.has_tess != has_tess || !(memo.start == start) ||
        !(memo.runtime_info == rt) ||
        std::memcmp(memo.user_data.data(), params.user_data.data(), sizeof(memo.user_data)) != 0) {
        return false;
    }
    if (has_tess && std::memcmp(&memo.tess, &tess, sizeof(tess)) != 0) {
        return false;
    }
    if (!std::ranges::equal(memo.flat_buf, program.info.FlatBuf())) {
        return false;
    }
    // sotc: the same test as ParseFetchShader: a vertex stage with a fetch shader.
    const bool fetch_now =
        program.info.sw_stage == Shader::SwStage::Vertex && program.info.has_fetch_shader;
    if (memo.has_fetch != fetch_now) {
        return false;
    }
    if (!fetch_now) {
        return true;
    }
    const u8* backing = Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(
        Shader::Gcn::GetFetchShaderCode(program.info, program.info.fetch_shader_sgpr_base)));
    return backing != nullptr &&
           (memo.fetch_code.empty() ||
            std::memcmp(backing, memo.fetch_code.data(), memo.fetch_code.size() * sizeof(u32)) == 0);
}

PipelineCache::StageMemo* PipelineCache::StageMemoVictim(StageMemo* ways, const Program& program,
                                                         Shader::HwStage hw_stage,
                                                         Shader::SwStage sw_stage, u64 hash) {
    StageMemo* pick = nullptr;
    for (size_t w = 0; w < kStageMemoWays; ++w) {
        StageMemo& m = ways[w];
        if (m.valid && m.program == &program && m.hw_stage == hw_stage &&
            m.sw_stage == sw_stage && m.hash == hash) {
            return &m; // same key, other inputs: this entry is replaced, nothing is evicted
        }
        // An empty way first, else the least recently used one.
        if (!pick || (!m.valid && pick->valid) || (m.valid && pick->valid && m.stamp < pick->stamp)) {
            pick = &m;
        }
    }
    if (pick->valid) {
        g_pipe_memo.stage_evictions.fetch_add(1, std::memory_order_relaxed);
    }
    return pick;
}

bool PipelineCache::StageMemoStore(StageMemo& memo, const Program& program,
                                   Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                                   const Shader::ShaderParams& params,
                                   const Shader::RuntimeInfo& rt,
                                   const Shader::Backend::Bindings& start,
                                   const Shader::TessellationDataConstantBuffer& tess,
                                   bool has_tess, u64 generation, size_t perm,
                                   const Shader::Backend::Bindings& after) {
    const bool fetch_now =
        program.info.sw_stage == Shader::SwStage::Vertex && program.info.has_fetch_shader;
    // Read before anything in the entry changes: a failed read leaves the old entry as it was.
    const u8* fetch_backing = nullptr;
    u32 fetch_words = 0;
    if (fetch_now) {
        fetch_backing = Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(
            Shader::Gcn::GetFetchShaderCode(program.info, program.info.fetch_shader_sgpr_base)));
        if (!fetch_backing) {
            return false;
        }
        fetch_words = program.modules[perm].spec.fetch_shader_data.size / sizeof(u32);
        memo.fetch_code.resize(fetch_words); // keeps its capacity once warm
        std::memcpy(memo.fetch_code.data(), fetch_backing, fetch_words * sizeof(u32));
    } else {
        memo.fetch_code.clear();
    }
    memo.valid = true;
    memo.program = &program;
    memo.hw_stage = hw_stage;
    memo.sw_stage = sw_stage;
    memo.hash = params.hash;
    memo.perm = static_cast<u32>(perm);
    memo.modules_size = program.modules.size();
    std::memcpy(memo.user_data.data(), params.user_data.data(), sizeof(memo.user_data));
    // assign() reuses the capacity kept from earlier stores: no allocation once warm.
    const auto& flat = program.info.FlatBuf();
    memo.flat_buf.assign(flat.begin(), flat.end());
    memo.runtime_info = rt;
    memo.start = start;
    memo.after = after;
    memo.has_tess = has_tess;
    memo.tess = tess;
    memo.has_fetch = fetch_now;
    memo.generation = generation;
    memo.stamp = ++stage_memo_tick;
    return true;
}

// sotc: writes a report line for the 5 s report: counts since the previous call. Prints nothing
// while SOTC_PIPE_MEMO is not 1 or 2, or when the window had no lookups. Call from one thread only.
void ReportPipeMemo(std::string& out, double seconds) {
    if (PipeMemoStageMode() == 0) {
        return;
    }
    // Same order as the fields of PipeMemoStats.
    const std::array<const std::atomic<u64>*, 15> counters{
        &g_pipe_memo.graphics_lookups,      &g_pipe_memo.graphics_hits,
        &g_pipe_memo.stage_lookups,         &g_pipe_memo.stage_hits,
        &g_pipe_memo.stage_evictions,       &g_pipe_memo.stage_foreign,
        &g_pipe_memo.compute_lookups,       &g_pipe_memo.compute_hits,
        &g_pipe_memo.mismatch_graphics,     &g_pipe_memo.mismatch_stage_perm,
        &g_pipe_memo.mismatch_stage_module, &g_pipe_memo.mismatch_stage_fetch,
        &g_pipe_memo.mismatch_stage_bindings, &g_pipe_memo.mismatch_stage_pending,
        &g_pipe_memo.mismatch_compute,
    };
    static std::array<u64, counters.size()> prev{};
    std::array<u64, counters.size()> d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        const u64 now = counters[i]->load(std::memory_order_relaxed);
        d[i] = now - prev[i];
        prev[i] = now;
    }
    // Indices as in the array above.
    constexpr std::size_t kGraphicsLookups = 0, kGraphicsHits = 1, kStageLookups = 2,
                          kStageHits = 3, kStageEvictions = 4, kStageForeign = 5,
                          kComputeLookups = 6, kComputeHits = 7, kMismatchGraphics = 8,
                          kMismatchPerm = 9, kMismatchModule = 10, kMismatchFetch = 11,
                          kMismatchBindings = 12, kMismatchPending = 13, kMismatchCompute = 14;
    if (d[kGraphicsLookups] == 0 && d[kStageLookups] == 0 && d[kComputeLookups] == 0 &&
        d[kStageForeign] == 0) {
        return;
    }
    const auto pct = [](u64 hits, u64 lookups) {
        return lookups ? 100.0 * static_cast<double>(hits) / static_cast<double>(lookups) : 0.0;
    };
    const u64 stage_mismatch = d[kMismatchPerm] + d[kMismatchModule] + d[kMismatchFetch] +
                               d[kMismatchBindings] + d[kMismatchPending];
    char line[640];
    std::snprintf(
        line, sizeof(line),
        "SOTC pipe memo: mode %d, graphics %llu/%llu hits (%.0f%%), stage %llu/%llu hits "
        "(%.0f%%), stage evictions %llu, stage foreign %llu, compute %llu/%llu hits (%.0f%%), "
        "mismatches %llu (graphics %llu, stage %llu: perm %llu, module %llu, fetch %llu, "
        "bindings %llu, pending %llu; compute %llu), %.1f lookups/s\n",
        PipeMemoStageMode(), static_cast<unsigned long long>(d[kGraphicsHits]),
        static_cast<unsigned long long>(d[kGraphicsLookups]), pct(d[kGraphicsHits], d[kGraphicsLookups]),
        static_cast<unsigned long long>(d[kStageHits]), static_cast<unsigned long long>(d[kStageLookups]),
        pct(d[kStageHits], d[kStageLookups]), static_cast<unsigned long long>(d[kStageEvictions]),
        static_cast<unsigned long long>(d[kStageForeign]),
        static_cast<unsigned long long>(d[kComputeHits]),
        static_cast<unsigned long long>(d[kComputeLookups]), pct(d[kComputeHits], d[kComputeLookups]),
        static_cast<unsigned long long>(d[kMismatchGraphics] + stage_mismatch + d[kMismatchCompute]),
        static_cast<unsigned long long>(d[kMismatchGraphics]), static_cast<unsigned long long>(stage_mismatch),
        static_cast<unsigned long long>(d[kMismatchPerm]), static_cast<unsigned long long>(d[kMismatchModule]),
        static_cast<unsigned long long>(d[kMismatchFetch]), static_cast<unsigned long long>(d[kMismatchBindings]),
        static_cast<unsigned long long>(d[kMismatchPending]), static_cast<unsigned long long>(d[kMismatchCompute]),
        seconds > 0.0
            ? static_cast<double>(d[kGraphicsLookups] + d[kStageLookups] + d[kComputeLookups]) / seconds
            : 0.0);
    out += line;
}

// sotc: a draw with a depth target, depth writes enabled and no color target bound (a shadow-map
// pass). The color rules are the ones RefreshGraphicsKey applies.
bool PipelineCache::IsDepthOnlyDraw() const {
    const auto& regs = liverpool->regs;
    if (!regs.depth_buffer.DepthValid() || !regs.depth_control.depth_write_enable) {
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) {
        return true;
    }
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
            return false;
        }
    }
    return true;
}

// sotc: SOTC_ADDR_FALLBACK, the 5 s report line: window counts of the fallback ops and the table pipelines built, and
// the table pipelines still pending. Nothing while the fallback is off or the window had nothing.
void ReportAddrFallback(std::string& out, double seconds) {
    (void)seconds;
    if (!g_addr_fb.on.load(std::memory_order_relaxed)) {
        return;
    }
    static u64 prev_draws{}, prev_dispatches{}, prev_ready{};
    const u64 draws = g_addr_fb.draws.load(std::memory_order_relaxed);
    const u64 dispatches = g_addr_fb.dispatches.load(std::memory_order_relaxed);
    const u64 ready = g_addr_fb.ready.load(std::memory_order_relaxed);
    const s64 pending = std::max<s64>(0, g_addr_fb.pending.load(std::memory_order_relaxed));
    const u64 d_draws = draws - prev_draws, d_dispatches = dispatches - prev_dispatches, d_ready = ready - prev_ready;
    prev_draws = draws;
    prev_dispatches = dispatches;
    prev_ready = ready;
    if (d_draws == 0 && d_dispatches == 0 && d_ready == 0 && pending == 0) {
        return;
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "SOTC ADDR FALLBACK: draws %llu, dispatches %llu (pending table pipelines %lld), table ready %llu "
                  "(total %llu)\n",
                  static_cast<unsigned long long>(d_draws), static_cast<unsigned long long>(d_dispatches),
                  static_cast<long long>(pending), static_cast<unsigned long long>(d_ready),
                  static_cast<unsigned long long>(ready));
    out += line;
}

bool PipelineCache::AddrFallbackOn() const {
    return addr_fallback && !async_workers.empty();
}

bool PipelineCache::AddrFallbackComputeOn() const {
    // sotc: the table compute pipeline is built on a worker and bound only once built; without deferred compute builds
    // (no record thread, shader collect) dispatches are as without the switch.
    return AddrFallbackOn() && AsyncComputeEnabled() && scheduler.HasRecordThread() &&
           !EmulatorSettings.IsShaderCollect();
}

bool PipelineCache::AddrTableGraphicsReady() {
    g_addr_fb.on.store(true, std::memory_order_relaxed);
    // The previous draw's pipeline (the key memo's test, without the probe): non-null only while it is that key's.
    if (last_graphics_pipeline && std::memcmp(&graphics_key, &last_graphics_key, sizeof(graphics_key)) == 0) {
        return true;
    }
    if (const auto it = graphics_pipelines.find(graphics_key); it != graphics_pipelines.end()) {
        return it->second != nullptr; // null: queued on a worker (a draw's, a preload's, or ours)
    }
    // sotc: a pipeline whose construction reads this draw's state cannot be built on a worker: its key keeps the
    // descriptor-mode pipeline (built as a descriptor-mode run builds it), never a table build here.
    if (!AsyncPipelineEligible()) {
        return false;
    }
    graphics_pipelines.try_emplace(graphics_key); // the null entry marks it queued, as QueueAsyncPipeline's callers do
    QueueAsyncPipeline(std::hash<GraphicsPipelineKey>{}(graphics_key), nullptr, false, true, true);
    g_addr_fb.pending.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool PipelineCache::AddrTableComputeReady() {
    g_addr_fb.on.store(true, std::memory_order_relaxed);
    if (const auto it = compute_pipelines.find(compute_key); it != compute_pipelines.end()) {
        return it->second && it->second->IsReady();
    }
    if (!infos[0]) {
        return false;
    }
    // sotc: made here (its layouts are CP-only) and built by a worker in the background lane. No dispatch binds it
    // before it is built, so the recorder never waits for it (BindWhenReady).
    ComputePipeline::SerializationSupport sdata{};
    auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, layout_cache,
                                                   StageProfile(infos[0]->variant), *pipeline_cache, compute_key,
                                                   *infos[0], modules[0], sdata, false, true);
    auto job = std::make_unique<AsyncPipelineJob>();
    job->compute = it.value().get();
    job->addr_table = true;
    {
        std::scoped_lock lock{async_mutex};
        PushJobLocked(JobLane::Background, std::move(job));
    }
    async_cv.notify_one();
    RegisterPipelineData(compute_key, sdata);
    ++num_new_pipelines;
    g_addr_fb.pending.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// sotc: SOTC_COMPUTE_FAST: the optimized build of a pipeline already bound with its fast build. Nothing waits for it, so
// it goes to the Background lane (capped, below normal priority under SOTC_NB_SCHED).
void PipelineCache::QueueComputeOptimized(ComputePipeline* pipeline) {
    auto job = std::make_unique<AsyncPipelineJob>();
    job->compute = pipeline;
    job->compute_opt = true;
    {
        std::scoped_lock lock{async_mutex};
        PushJobLocked(JobLane::Background, std::move(job));
    }
    async_cv.notify_one();
}

// sotc: PipelineKeyMemo. The pipeline the last whole graphics lookup returned (null: none) and its indirect parameters;
// the memo in GetGraphicsPipelineLookupSlow returns it only while it is also last_graphics_pipeline. Command processor
// thread only (as the rest of the lookup state).
static const GraphicsPipeline* g_key_memo_pipeline = nullptr;
static DrawIndirectParams g_key_memo_params{};

static const GraphicsPipeline* NoteKeyMemo(const GraphicsPipeline* pipeline, const DrawIndirectParams& params) {
    g_key_memo_pipeline = pipeline;
    g_key_memo_params = params;
    return pipeline;
}

// sotc: N5. Counts the draws that bind each pipeline (its use count, saved at teardown).
const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params) {
    const GraphicsPipeline* pipeline = GetGraphicsPipelineLookup(params);
    if (pipeline) {
        if (addr_fallback && &pipeline->GetProfile() == &desc_profile) { // sotc: SOTC_ADDR_FALLBACK
            g_addr_fb.draws.fetch_add(1, std::memory_order_relaxed);
        }
        ++pipeline->use_count;
        if (!pipeline->recent_noted && SotcNonBlocking::LazyPso()) { // sotc: once per pipeline object
            pipeline->recent_noted = true;
            NoteRecentPipeline(std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey()));
        }
    }
    return pipeline;
}

// sotc: SOTC_PIPE_FAST: the full lookup; GetGraphicsPipelineLookup runs it unless a record applies.
const GraphicsPipeline* PipelineCache::GetGraphicsPipelineLookupSlow(const DrawIndirectParams params) {
    draw_indirect_params = params;
    // sotc: SOTC_PIPE_MANIFEST: the boot hold (once), then the manifest stages whose code came since. Before the key
    // is built: installs change the program cache.
    if (SotcPipeManifest::On()) {
        MaybeHoldForManifest();
    }
    // sotc: SOTC_DEPTH_SYNC_PIPE (default on since SOTC_PIPE_MANIFEST; =0 off) builds depth-only (shadow) pipelines
    // on this thread so their draws are never skipped (a skipped depth-only draw leaves its pass without shadows or
    // its depth pre-pass missing: dark scene, fog at full strength). It was off from 2026-10-27 to the manifest (each
    // build took ~36 ms and an area change built several, hitches of 0.4-3.8 s): the manifest builds the known ones at
    // the boot hold, so only keys it does not hold are built here. =0 queues them ahead of the other pipelines
    // instead (QueueAsyncPipeline priority) and skips their draws meanwhile.
    static const bool depth_sync = [] {
        const char* v = std::getenv("SOTC_DEPTH_SYNC_PIPE");
        return !v || v[0] != '0';
    }();
    const bool depth_draw = !depth_sync && IsDepthOnlyDraw();
    // sotc: shadow casters are depth-only geometry draws. Skipping them while their pipeline is
    // built made shadows blink after loads, so they are never skipped: they build here, like the
    // full-screen passes. Their shaders are trivial, so building inline is cheap.
    const bool depth_only = depth_sync && IsDepthOnlyDraw();
    // sotc: SOTC_PIPE_GPL: a draw that finds no built pipeline is served by a fast-linked one.
    const bool gpl_on = instance.IsGraphicsPipelineLibrary();
    // sotc: finished translations must be installed before the stages are looked up.
    if (async_done_count.load(std::memory_order_acquire) != 0) {
        CollectAsyncPipelines();
    }
    // sotc: SOTC_PIPE_PREDICT: the materials' pipelines, queued before their first draw (no draw state is used).
    if (SotcPipePredict::On() && !async_workers.empty()) {
        PredictPipelines();
    }
    // sotc: SOTC_GPL_PREDICT: the libraries of the materials' programs, queued before their first draw (with
    // SOTC_PIPE_PREDICT on, PredictPipelines hands its slots on instead).
    if (SotcNonBlocking::GplPredict() && !SotcPipePredict::On() && !async_workers.empty()) {
        PredictLibraries(nullptr);
    }
    // sotc: SOTC_NB_GPL (P4): libraries of new shaders, built before their first draw.
    if (SotcNonBlocking::Gpl() && SotcNonBlocking::Warm() && !async_workers.empty()) {
        FeedWarmLibraries();
    }
    // sotc: full-screen passes (tiny draws, see below) are never skipped, so their shaders are
    // translated here. Without dynamic vertex input the key needs the vertex shader's sharps.
    async_translate_ok = !async_workers.empty() && AsyncTranslateEnabled() &&
                         liverpool->regs.num_indices > 6 && instance.IsVertexInputDynamicState() &&
                         !EmulatorSettings.IsShaderCollect() && !depth_only;
    translation_pending = false;
    // sotc: PipelineKeyMemo (BbToggle::PipelineKeyMemo disables it, SOTC_PIPE_KEY_MEMO=0 too):
    // RefreshGraphicsKey only reads the registers and the draw's indirect parameters, so a draw
    // that wrote no register chunk and passes the same parameters would rebuild exactly the key
    // of the previous draw and then hit the pipe memo below anyway. Return its pipeline without
    // walking the shader stages (GetProgram, specializations, flat buffers: ~15% of the command
    // processor at the Shrine). Without the draw thread MarkRegs does not track chunks.
    static const bool key_memo_env = [] {
        const char* v = std::getenv("SOTC_PIPE_KEY_MEMO");
        return !v || v[0] != '0';
    }();
    // sotc: the memo holds only the pipeline the last whole lookup returned (g_key_memo_pipeline, set by
    // GetGraphicsPipelineLookup) while it is still last_graphics_pipeline (every install or erase that may replace it
    // clears that), so no early return of this lookup can leave an older key's pipeline behind. Not with
    // SOTC_ADDR_FALLBACK: a descriptor-mode draw must look for its table pipeline again.
    if (key_memo_env && !BbToggle::Disabled(BbToggle::PipelineKeyMemo) && last_graphics_pipeline &&
        g_key_memo_pipeline == last_graphics_pipeline && !AddrFallbackOn() &&
        liverpool->IsBackThreaded() && !liverpool->HasDirtyRegChunks() &&
        params.vertex_sgpr_offset == g_key_memo_params.vertex_sgpr_offset &&
        params.instance_sgpr_offset == g_key_memo_params.instance_sgpr_offset) {
        if (SotcStats::kFpsLog) {
            SotcStats::pipe_key_memo_hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (last_graphics_pipeline->IsLinked()) {
            g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
        }
        return last_graphics_pipeline;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::pipe_key_memo_misses.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 key_start = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
    // sotc: SOTC_ADDR_FALLBACK. The table key first. Its stages are never translated here (a stage not translated yet
    // goes to a worker, for every draw kind), and when its pipeline is not ready (AddrTableGraphicsReady queues it),
    // the key is made again with the descriptor-mode stages, which the rest of this lookup builds or finds by the rules
    // of a descriptor-mode run (the same async_translate_ok, skips, tiny and depth builds).
    addr_fallback_draw = false;
    bool keyed;
    if (AddrFallbackOn()) {
        const bool draw_async_ok = async_translate_ok;
        async_translate_ok = AsyncTranslateEnabled() && instance.IsVertexInputDynamicState() &&
                             !EmulatorSettings.IsShaderCollect();
        keyed = RefreshGraphicsKey();
        const bool table_pending = !keyed && translation_pending;
        if (table_pending || (keyed && !AddrTableGraphicsReady())) {
            addr_fallback_draw = true;
            async_translate_ok = draw_async_ok;
            translation_pending = false;
            keyed = RefreshGraphicsKey();
        } else {
            async_translate_ok = draw_async_ok;
        }
    } else {
        keyed = RefreshGraphicsKey();
    }
    // sotc: SOTC_GPL_NODROP: the stages still on a translation worker (or queued there by this lookup) are translated
    // here instead of dropping the draw, as a tiny or depth-only draw's always were; the worker's copy is dropped when
    // installed (InstallTranslation finds the program or permutation made). Not in table mode (SOTC_ADDR_FALLBACK has
    // its own descriptor-mode fallback).
    const bool nodrop = SotcNonBlocking::GplNoDrop();
    if (!keyed && translation_pending && nodrop && !AddrFallbackOn()) {
        const SotcBlockLedger::Scope ledger{SotcBlockLedger::StageDemand}; // sotc: cold fallback
        const auto xlate_start = std::chrono::steady_clock::now();
        async_translate_ok = false;
        translation_pending = false;
        keyed = RefreshGraphicsKey();
        g_nodrop.sync_xlate.fetch_add(1, std::memory_order_relaxed);
        g_nodrop.sync_xlate_ns.fetch_add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - xlate_start)
                                 .count()),
            std::memory_order_relaxed);
        LogSyncPipeline("stages", "translation pending", xlate_start, infos[3] ? infos[3]->pgm_hash : 0,
                        infos[0] ? infos[0]->pgm_hash : 0);
    }
    if (!keyed) {
        if (translation_pending) {
            SotcStats::async_translate_skips.fetch_add(1, std::memory_order_relaxed);
            if (nodrop) {
                g_nodrop.drop_translate.fetch_add(1, std::memory_order_relaxed);
            }
        }
        last_graphics_pipeline = nullptr; // sotc: the key is not usable for the next draw
        return nullptr;
    }
    if (key_start) {
        SotcStats::pipe_key_ns.fetch_add(SotcStats::NowNs() - key_start,
                                         std::memory_order_relaxed);
        SotcStats::pipe_key_draws.fetch_add(1, std::memory_order_relaxed);
    }
    // sotc: consecutive draws mostly use the same pipeline: when the freshly built key equals the
    // previous draw's byte for byte, skip the map's hash and probe. The stage state (infos,
    // modules, fetch shader) was refreshed by RefreshGraphicsKey either way. SOTC_PIPE_MEMO=0.
    const int memo_mode = PipeMemoStageMode();
    // The map size check (an erase since the last store) is only made in modes 1 and 2.
    const bool key_hit = PipeMemoKeyOn() && last_graphics_pipeline &&
                         (memo_mode == 0 || last_graphics_size == graphics_pipelines.size()) &&
                         std::memcmp(&graphics_key, &last_graphics_key, sizeof(graphics_key)) == 0;
    if (memo_mode != 0) {
        g_pipe_memo.graphics_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    if (key_hit && memo_mode != 2) {
        if (memo_mode != 0) {
            g_pipe_memo.graphics_hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (last_graphics_pipeline->IsLinked()) {
            g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
        }
        if (SotcStats::kFpsLog) {
            SotcStats::pipe_same_key_hits.fetch_add(1, std::memory_order_relaxed);
        }
        last_graphics_params = params;
        return last_graphics_pipeline;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::pipe_same_key_misses.fetch_add(1, std::memory_order_relaxed);
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (pack_ref.undrawn != 0) { // sotc: SOTC_PACK_REFERENCED: a late library's program drawn for the first time
        NotePackRefDraw();
    }
    if (pipe_predict) { // sotc: SOTC_PIPE_PREDICT: templates and hits
        NotePredictDraw(is_new, !is_new && it->second != nullptr);
    }
    if (is_new && lib_predict) { // sotc: SOTC_GPL_PREDICT: the pass state of the predicted library jobs
        NoteLibraryTemplate();
    }
    if (is_new && SotcNonBlocking::Gpl() && SotcNonBlocking::Warm()) { // sotc: SOTC_NB_WARM (P4): the template
        NoteWarmTemplate();
    }
    if (key_hit) { // =2 only: the slow path runs too and must give the same pipeline
        g_pipe_memo.graphics_hits.fetch_add(1, std::memory_order_relaxed);
        if (is_new || it.value().get() != last_graphics_pipeline) {
            g_pipe_memo.mismatch_graphics.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // sotc: still being built by a pipeline worker (SOTC_ASYNC_SHADERS, SOTC_ASYNC_PRELOAD). A
    // full-screen pass is not skipped: it builds the pipeline here, and the worker's copy is
    // dropped when collected.
    const bool queued = !is_new && !it->second;
    if (gpl_on && (is_new || queued)) {
        g_gpl_counters.cold_misses.fetch_add(1, std::memory_order_relaxed);
    }
    // sotc: SOTC_NB_SCHED: one more draw is waiting for this key's queued job.
    const auto note_skip_demand = [this] {
        if (!SotcNonBlocking::Sched()) {
            return;
        }
        if (const auto f = queued_graphics.find(graphics_key); f != queued_graphics.end()) {
            f->second->demand.fetch_add(1, std::memory_order_relaxed);
            g_sched_demand.fetch_add(1, std::memory_order_relaxed);
        }
    };
    // sotc: SOTC_GPL_NODROP: a queued geometry draw is linked (missing libraries built here); when the link cannot
    // serve it, the full pipeline is built below on this thread. Never skipped.
    const bool nodrop_queued_linked =
        nodrop && queued && liverpool->regs.num_indices > 6 && !depth_only && NoDropLinkable() &&
        NoDropLink(it.value());
    if (nodrop_queued_linked) {
        std::memcpy(&last_graphics_key, &graphics_key, sizeof(graphics_key));
        last_graphics_pipeline = it->second.get();
        last_graphics_size = graphics_pipelines.size();
        g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
        return it->second.get();
    }
    if (!nodrop && queued && liverpool->regs.num_indices > 6 && !depth_only) {
        // sotc: SOTC_PIPE_GPL: the draw runs on a fast-linked pipeline; the queued build replaces it
        // when it is collected (CollectAsyncPipelines).
        if (!gpl_on || !TryFastLink(it.value())) {
            if (gpl_on && SotcNonBlocking::Gpl() && !link_pending.contains(graphics_key)) {
                QueueLinkJob(); // sotc: SOTC_NB_GPL, the libraries are built off the command processor
            }
            note_skip_demand();
            SotcStats::pipeline_pending_drops.fetch_add(1, std::memory_order_relaxed); // sotc: the 5 s line
            last_graphics_pipeline = nullptr; // sotc: key memo off until one is installed
            return nullptr;
        }
        std::memcpy(&last_graphics_key, &graphics_key, sizeof(graphics_key));
        last_graphics_pipeline = it->second.get();
        last_graphics_size = graphics_pipelines.size();
        g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
        return it->second.get();
    }
    if (is_new || queued) {
        const bool eligible = AsyncPipelineEligible();
        // sotc: SOTC_TINY_ASYNC. Full-screen passes (post-processing, composition) are tiny draws:
        // skipping one leaves a black or stale frame, so by default they still build here (see
        // TinyAsyncMode for when mode 1 defers them).
        const bool tiny_draw = liverpool->regs.num_indices <= 6;
        // sotc: SOTC_GPL_NODROP: no tiny draw is deferred (the skips of SOTC_TINY_ASYNC are off).
        const int tiny_mode = tiny_draw && !depth_only && !nodrop ? TinyAsyncMode() : 0;
        const bool tiny_can_defer = tiny_mode != 0 && !async_workers.empty() && eligible;
        // sotc: a color target lies in a display buffer (a presentation pass): never deferred.
        const auto tiny_writes_display = [this] {
            const auto& regs = liverpool->regs;
            std::scoped_lock lock{g_display_mutex};
            if (g_display_ranges.empty()) {
                return true; // no display buffer known: every draw counts as one
            }
            for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
                if (!regs.color_buffers[cb] || !regs.color_target_mask.GetMask(cb)) {
                    continue;
                }
                const u64 addr = regs.color_buffers[cb].Address();
                for (const auto& [begin, end] : g_display_ranges) {
                    if (addr >= begin && addr < end) {
                        return true;
                    }
                }
            }
            return false;
        };
        // sotc: moves this draw's queued pipeline to the tiny run: behind the compute and
        // translation jobs, ahead of the priority jobs. Returns false if no worker has it queued
        // (a worker may have taken it already) or it is in the run already.
        const auto move_to_tiny_run = [this] {
            std::scoped_lock lock{async_mutex};
            // A queued graphics or shadow job of this key moves to the tiny lane (after compute and translate).
            for (const JobLane from : {JobLane::Graphics, JobLane::Shadow}) {
                auto& q = job_queues[from];
                const auto found = std::find_if(q.begin(), q.end(),
                                                [this](const auto& job) { return job->key == graphics_key; });
                if (found == q.end()) {
                    continue;
                }
                auto job = std::move(*found);
                q.erase(found);
                job->priority = false;
                PushJobLocked(JobLane::Tiny, std::move(job));
                return true;
            }
            return false;
        };
        // sotc: SOTC_TINY_GPL=1 (TinyGplOn). A tiny draw is served by a fast-linked pipeline now, and the
        // optimized build follows on a worker. Its time is counted in g_tiny_gpl.
        // Descriptor heap pipelines are never linked (GraphicsPipeline ctor): they would be built twice.
        // sotc: SOTC_GPL_NODROP takes this path too (NoDropLink: the same link, counted in its line).
        if (tiny_draw && !depth_only && (TinyGplOn() || SotcNonBlocking::Gpl() || nodrop) && gpl_on && eligible &&
            !async_workers.empty() && !instance.IsDescriptorHeapEnabled()) {
            const auto fast_start = std::chrono::steady_clock::now();
            const u64 link_before = g_gpl_counters.link_ns.load(std::memory_order_relaxed);
            if (nodrop ? NoDropLinkable() && NoDropLink(it.value()) : TryFastLink(it.value(), true)) {
                if (SotcNonBlocking::Gpl()) {
                    RecordTinyPipeline(); // sotc: built before the first draw from the next boot hold on
                }
                const u64 link_ns =
                    g_gpl_counters.link_ns.load(std::memory_order_relaxed) - link_before;
                const u64 total_ns = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - fast_start)
                        .count());
                g_tiny_gpl.linked.fetch_add(1, std::memory_order_relaxed);
                g_tiny_gpl.link_ns.fetch_add(link_ns, std::memory_order_relaxed);
                g_tiny_gpl.lib_ns.fetch_add(total_ns > link_ns ? total_ns - link_ns : 0,
                                            std::memory_order_relaxed);
                // The optimized build: queued for a new pipeline (a queued one is already in flight).
                if (!queued) {
                    QueueAsyncPipeline(std::hash<GraphicsPipelineKey>{}(graphics_key), nullptr,
                                       false);
                    g_tiny_gpl.queued.fetch_add(1, std::memory_order_relaxed);
                }
                std::memcpy(&last_graphics_key, &graphics_key, sizeof(graphics_key));
                last_graphics_pipeline = it->second.get();
                last_graphics_size = graphics_pipelines.size();
                g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
                return it->second.get();
            }
            // Not linkable, or refused: the full build below, as without the switch.
        }
        // sotc: SOTC_TINY_ASYNC=1: a tiny draw whose pipeline a worker is building is skipped, as
        // a geometry draw is, and the pipeline moves to the front. A display draw builds here.
        if (queued && tiny_mode == 1 && tiny_can_defer && !tiny_writes_display()) {
            if (move_to_tiny_run()) {
                g_tiny_stats.moved.fetch_add(1, std::memory_order_relaxed);
            }
            g_tiny_stats.skipped.fetch_add(1, std::memory_order_relaxed);
            note_skip_demand();
            return nullptr;
        }
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        // Geometry draws are skipped while their pipeline is built (a brief pop-in instead of a
        // freeze on first sight).
        // sotc: SOTC_GPL_NODROP: only a draw the link covers is queued here; any other builds below on this thread.
        if (!async_workers.empty() && !tiny_draw && !depth_only && eligible && !queued &&
            (!nodrop || NoDropLinkable())) {
            // sotc: at the front of its lane: a draw waits for it, ahead of the background builds (the boot preload,
            // the manifest's late pipelines) queued in the same lane.
            QueueAsyncPipeline(pipeline_hash, nullptr, depth_draw, false, false, true);
            // sotc: SOTC_PIPE_GPL: the draw runs on a fast-linked pipeline now (not registered here:
            // the optimized build registers when it replaces it).
            // sotc: SOTC_GPL_NODROP: a refused link builds the full pipeline below (registered there; the copy of the
            // worker is dropped when collected, as for a full-screen pass).
            const bool linked_now = nodrop ? NoDropLink(it.value()) : gpl_on && TryFastLink(it.value());
            if (!linked_now && !nodrop) {
                if (gpl_on && SotcNonBlocking::Gpl()) {
                    QueueLinkJob(); // sotc: SOTC_NB_GPL, the libraries are built off the command processor
                }
                SotcStats::pipeline_pending_drops.fetch_add(1, std::memory_order_relaxed); // sotc: the 5 s line
                return nullptr; // the draws that need it are skipped until it is ready
            }
            if (linked_now) {
                std::memcpy(&last_graphics_key, &graphics_key, sizeof(graphics_key));
                last_graphics_pipeline = it->second.get();
                last_graphics_size = graphics_pipelines.size();
                g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
                return it->second.get();
            }
        }
        // sotc: SOTC_GPL_NODROP: a depth-only draw is linked when it can be (its optimized build queued at the front
        // of the shadow lane) instead of the full build below (SOTC_DEPTH_SYNC_PIPE, kept for the rest).
        if (nodrop && depth_only && NoDropLinkable() && NoDropLink(it.value())) {
            if (!queued) {
                QueueAsyncPipeline(pipeline_hash, nullptr, true, false, false, true);
            }
            std::memcpy(&last_graphics_key, &graphics_key, sizeof(graphics_key));
            last_graphics_pipeline = it->second.get();
            last_graphics_size = graphics_pipelines.size();
            g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
            return it->second.get();
        }
        // sotc: SOTC_TINY_ASYNC. Mode 2 only counts and logs what mode 1 would defer. Mode 1
        // queues a new pipeline at the front of the tiny run (a queued one returned above).
        if (tiny_can_defer) {
            const bool tiny_display = tiny_writes_display();
            if (tiny_mode == 2) {
                if (tiny_display) {
                    g_tiny_stats.would_defer_display.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_tiny_stats.would_defer.fetch_add(1, std::memory_order_relaxed);
                    if (g_tiny_logged.fetch_add(1, std::memory_order_relaxed) < 20) {
                        std::fprintf(stderr, "SOTC tiny draw would defer %016llx%s\n",
                                     static_cast<unsigned long long>(pipeline_hash),
                                     queued ? " (already queued)" : "");
                    }
                }
            } else if (tiny_display) {
                g_tiny_stats.display_inline.fetch_add(1, std::memory_order_relaxed);
            } else {
                QueueAsyncPipeline(pipeline_hash, nullptr, false);
                // Queued at the back by QueueAsyncPipeline, moved to the front right after. If a
                // worker took it in between, the draw is skipped all the same.
                move_to_tiny_run();
                g_tiny_stats.queued.fetch_add(1, std::memory_order_relaxed);
                g_tiny_stats.skipped.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        }
        const SotcStats::ScopedTimer stall_timer{SotcStats::pipeline_ns};
        SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);
        SotcFrameMetrics::AddCurrent(SotcFrameMetrics::PipelineCompiles, 1); // sotc: SOTC_FRAME_METRICS
        const auto sync_start = std::chrono::steady_clock::now();

        const SotcStats::FrameTimer frame_timer{SotcStats::FramePipeline};
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
        GraphicsPipeline::SerializationSupport sdata{};
        {
            const SotcBlockLedger::Scope ledger{SotcBlockLedger::PipeBuild};
            it.value() = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, layout_cache, StagesProfile(infos), graphics_key,
                *pipeline_cache, infos,
                runtime_infos, fetch_shader, modules, sdata, false);
        }
        // sotc: SOTC_NB_SCHED: a worker that has not started this key's job skips its build.
        if (queued && SotcNonBlocking::Sched()) {
            if (const auto f = queued_graphics.find(graphics_key); f != queued_graphics.end()) {
                f->second->cancelled.store(true, std::memory_order_release);
            }
        }
        // sotc: SOTC_GPL_NODROP: whether the fast link covers this state (the counters and the reason of the log).
        const bool linkable =
            nodrop && GraphicsPipeline::FastLinkable(graphics_key, infos, runtime_infos, StagesProfile(infos));
        if (nodrop) {
            g_nodrop.sync_full.fetch_add(1, std::memory_order_relaxed);
            g_nodrop.sync_full_ns.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - sync_start)
                                     .count()),
                std::memory_order_relaxed);
            if (!linkable) {
                g_nodrop.sync_full_unlinkable.fetch_add(1, std::memory_order_relaxed);
            }
        }
        LogSyncPipeline("graphics",
                        async_workers.empty()     ? "async off"
                        : nodrop && !linkable     ? (tiny_draw    ? "tiny draw, not linkable"
                                                     : depth_only ? "depth only, not linkable"
                                                                  : "not linkable")
                        : tiny_draw               ? "tiny draw"
                        : depth_only              ? "depth only"
                        : AsyncIneligibleReason() ? AsyncIneligibleReason()
                        : nodrop                  ? "link refused"
                                                  : "not eligible",
                        sync_start, infos[3] ? infos[3]->pgm_hash : 0, // vertex
                        infos[0] ? infos[0]->pgm_hash : 0);              // fragment
        N5NoteSyncCompile(N5Kind::Graphics, sync_start); // sotc: N5

        if (tiny_draw) { // sotc: SOTC_TINY_ASYNC counters (every mode), for the 5 s report
            g_tiny_stats.sync_builds.fetch_add(1, std::memory_order_relaxed);
            g_tiny_stats.sync_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - sync_start)
                    .count(),
                std::memory_order_relaxed);
        }
        if (!queued) { // a queued one registers when collected (or was preloaded)
            RegisterPipelineData(graphics_key, pipeline_hash, sdata);
            ++num_new_pipelines;
        }

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (infos[stage]) {
                    auto& m = modules[stage];
                    module_related_pipelines[m].emplace_back(graphics_key);
                }
            }
        }
    }
    std::memcpy(&last_graphics_key, &graphics_key, sizeof(graphics_key));
    last_graphics_pipeline = it->second.get();
    last_graphics_size = graphics_pipelines.size();
    // sotc: SOTC_PIPE_MANIFEST: the pipeline's first full lookup this session: recorded when no manifest holds it.
    if (manifest && it->second && !it->second->manifest_noted) {
        it->second->manifest_noted = CaptureManifestGraphics();
    }
    last_graphics_params = params;
    return it->second.get();
}

// sotc: N5. Counts the dispatches that bind each pipeline (its use count, saved at teardown).
const ComputePipeline* PipelineCache::GetComputePipeline() {
    const ComputePipeline* pipeline = GetComputePipelineLookup();
    if (pipeline) {
        if (addr_fallback && &pipeline->GetProfile() == &desc_profile) { // sotc: SOTC_ADDR_FALLBACK
            g_addr_fb.dispatches.fetch_add(1, std::memory_order_relaxed);
        }
        ++pipeline->use_count;
        if (!pipeline->recent_noted && SotcNonBlocking::LazyPso()) { // sotc: once per pipeline object
            pipeline->recent_noted = true;
            NoteRecentPipeline(pipeline->KeyValue());
        }
    }
    return pipeline;
}

namespace {
// sotc: SOTC_NB_LAZY_PSO recently used pipelines, in the versioned cache folder (the ids are this version's keys).
// Header: magic, the session that wrote it, the count; then (id, session of the last use).
constexpr u32 kRecentMagic = 0x54435352; // "RSCT"
constexpr u32 kRecentSessions = 3;       // a pipeline unused for this many sessions is no longer built at boot
std::filesystem::path RecentPsoPath() {
    return Storage::DataBase::Instance().CachePath() / "sotc_recent_pso.bin";
}
} // namespace

void PipelineCache::LoadRecentPipelines() {
    recent_pso.clear();
    recent_session = 1;
    if (std::FILE* f = _wfopen(RecentPsoPath().c_str(), L"rb")) {
        u32 header[3]{};
        if (std::fread(header, sizeof header, 1, f) == 1 && header[0] == kRecentMagic) {
            recent_session = header[1] + 1;
            for (u32 i = 0; i < header[2]; ++i) {
                u64 id{};
                u32 session{};
                if (std::fread(&id, sizeof id, 1, f) != 1 || std::fread(&session, sizeof session, 1, f) != 1) {
                    break;
                }
                if (session + kRecentSessions > recent_session) {
                    recent_pso.emplace(id, session);
                }
            }
        }
        std::fclose(f);
    }
    recent_saved = std::chrono::steady_clock::now();
    std::fprintf(stderr, "SOTC NB LAZY PSO: session %u, %zu pipelines used in the last %u sessions\n", recent_session,
                 recent_pso.size(), kRecentSessions);
}

bool PipelineCache::RecentPipeline(u64 id) const {
    return recent_pso.contains(id);
}

void PipelineCache::NoteRecentPipeline(u64 id) {
    if (recent_session == 0) {
        return; // the warm-up did not run (no cache): nothing to keep
    }
    auto& session = recent_pso[id];
    if (session != recent_session) {
        session = recent_session;
        recent_dirty = true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!recent_dirty || now - recent_saved < std::chrono::seconds(5)) {
        return;
    }
    recent_saved = now;
    recent_dirty = false;
    // A few tens of KB, written by a detached thread (no file I/O on the command processor).
    std::vector<u8> data;
    const auto put = [&data](const void* p, size_t n) {
        data.insert(data.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
    };
    const u32 header[3] = {kRecentMagic, recent_session, static_cast<u32>(recent_pso.size())};
    put(header, sizeof header);
    for (const auto& [pso, last] : recent_pso) {
        put(&pso, sizeof pso);
        put(&last, sizeof last);
    }
    std::thread([path = RecentPsoPath(), data = std::move(data)] {
        auto tmp = path;
        tmp += ".tmp";
        if (std::FILE* f = _wfopen(tmp.c_str(), L"wb")) {
            const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            std::fclose(f);
            std::error_code ec;
            if (ok) {
                std::filesystem::rename(tmp, path, ec);
            } else {
                std::filesystem::remove(tmp, ec);
            }
        }
    }).detach();
}

// sotc: SOTC_PIPE_FAST: the full lookup; GetComputePipelineLookup runs it unless a record applies.
const ComputePipeline* PipelineCache::GetComputePipelineLookupSlow() {
    if (SotcPipeManifest::On()) { // sotc: SOTC_PIPE_MANIFEST (see GetGraphicsPipelineLookupSlow)
        MaybeHoldForManifest(true); // sotc: SOTC_MANIFEST_HOLD_FAST: compute (only installs finished translations)
    }
    // sotc: SOTC_ADDR_FALLBACK. The table key first (RefreshComputeKey queues a missing table translation on a worker);
    // when its stage is pending or its pipeline not ready (AddrTableComputeReady queues it), the dispatch takes the
    // descriptor-mode stage and pipeline, found or built below as a descriptor-mode run does.
    addr_fallback_draw = false;
    if (AddrFallbackComputeOn()) {
        // Finished table translations are installed here too, or a stretch of dispatches without draws would keep the
        // descriptor pipeline. As WaitPendingTranslation does: a refused preloaded pipeline may be erased, and the
        // graphics record may point into a program whose permutations move.
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            last_graphics_pipeline = nullptr;
            pipe_fast_gfx.valid = false;
            CollectAsyncPipelines();
        }
        translation_pending = false;
        RefreshComputeKey();
        if (translation_pending || !AddrTableComputeReady()) {
            addr_fallback_draw = true;
            translation_pending = false;
            RefreshComputeKey();
        }
    } else if (!RefreshComputeKey()) {
        return nullptr;
    }
    // sotc: SOTC_PIPE_MEMO=1/2: the key of the last lookup and its pipeline. Same key, same map
    // entry: the map is probed only when the key changes or the maps changed size (an erase).
    const int memo_mode = PipeMemoStageMode();
    const bool memo_hit = memo_mode != 0 && last_compute_pipeline &&
                          compute_key == last_compute_key &&
                          last_compute_size == compute_pipelines.size();
    if (memo_mode != 0) {
        g_pipe_memo.compute_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    if (memo_hit) {
        g_pipe_memo.compute_hits.fetch_add(1, std::memory_order_relaxed);
        if (memo_mode == 1) {
            return last_compute_pipeline;
        }
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        const SotcStats::ScopedTimer stall_timer{SotcStats::pipeline_ns};
        SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);
        SotcFrameMetrics::AddCurrent(SotcFrameMetrics::PipelineCompiles, 1); // sotc: SOTC_FRAME_METRICS

        const SotcStats::FrameTimer frame_timer{SotcStats::FramePipeline};
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
        ComputePipeline::SerializationSupport sdata{};
        // Shader collection replaces modules and erases their pipelines: built here then.
        // Without a recording thread the command processor itself would wait for the worker.
        // The presenter's Finish at shutdown runs every deferred bind before the workers stop.
        const bool deferred = !async_workers.empty() && AsyncComputeEnabled() &&
                              scheduler.HasRecordThread() && !EmulatorSettings.IsShaderCollect();
        // sotc: SOTC_COMPUTE_PACK: a pipeline the pack list names is in the pack cache (a hit of ~1 ms): it is built
        // from that cache, and a deferred one by the recorder at its first bind, without the worker round trip.
        const bool pack_listed = SotcNonBlocking::ComputePack() && PackComputeListed(compute_key.value);
        // sotc: SOTC_COMPILE_STREAM: a released pipeline its dispatch needs before any rebuild: built from the pack
        // cache (a hit), on a worker as any dispatch's, and counted (the gap of the stream: the recorder waits for it).
        const bool stream_miss = !stream_released_keys.empty() && stream_released_keys.erase(compute_key.value) != 0;
        if (stream_miss) {
            ++stream_inline;
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::fprintf(stderr, "SOTC COMPILE STREAM: dispatch rebuilds released compute pipeline %#llx itself\n",
                             static_cast<unsigned long long>(compute_key.value));
            }
            StreamLog(false);
        }
        const auto sync_start = std::chrono::steady_clock::now();
        {
            const SotcBlockLedger::Scope ledger{SotcBlockLedger::ComputeBuild};
            it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap,
                                                           layout_cache, StageProfile(infos[0]->variant),
                                                           pack_listed || stream_miss ? PackPipelineCache()
                                                                                      : *pipeline_cache,
                                                           compute_key, *infos[0],
                                                           modules[0], sdata, false, deferred);
        }
        // sotc: SOTC_COMPUTE_SPECFREE: a pipeline compiled by this lookup is not a prebuilt hit (the pack cache's is).
        it.value()->sotc_dispatch_built = !pack_listed;
        if (deferred && pack_listed) {
            it.value()->sotc_inline = true;
        } else if (deferred) {
            auto job = std::make_unique<AsyncPipelineJob>();
            job->compute = it.value().get();
            // sotc: SOTC_COMPUTE_FAST: the dispatch's recorder will wait for it, so its first build is the fast one,
            // on the Urgent lane (the reserved workers and every general one take it first).
            job->compute_fast = SotcNonBlocking::ComputeFast();
            {
                std::scoped_lock lock{async_mutex};
                // Ahead of the graphics jobs, after the older compute ones: the recording
                // thread binds them in this order.
                PushJobLocked(job->compute_fast ? JobLane::Urgent : JobLane::Compute, std::move(job));
            }
            async_cv.notify_one();
        } else {
            LogSyncPipeline("compute", async_workers.empty() ? "async off" : "not deferred",
                            sync_start, infos[0]->pgm_hash, 0);
            N5NoteSyncCompile(N5Kind::Compute, sync_start); // sotc: N5
        }
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    } else if (it.value()->sotc_rehydrated) {
        // sotc: SOTC_COMPILE_STREAM: the first lookup of a rebuilt pipeline; late when its worker build is not done.
        it.value()->sotc_rehydrated = false;
        if (!it.value()->IsReady()) {
            ++stream_late;
            StreamLog(false);
        }
    }
    const ComputePipeline* result = it->second.get();
    if (memo_mode != 0) {
        if (memo_hit && result != last_compute_pipeline) { // =2 only
            g_pipe_memo.mismatch_compute.fetch_add(1, std::memory_order_relaxed);
        }
        last_compute_pipeline = result;
        last_compute_key = compute_key;
        last_compute_size = compute_pipelines.size();
    }
    // sotc: SOTC_PIPE_MANIFEST: the pipeline's first full lookup this session: recorded when no manifest holds it.
    if (manifest && result && !result->manifest_noted) {
        result->manifest_noted = CaptureManifestCompute();
    }
    return result;
}

// sotc: SOTC_PIPE_DIRTY. Unset or 0: the register part of the graphics key is rebuilt on every
// draw, as before. 1: it is rebuilt only when the registers it reads changed (PipeKeyRegs), else
// taken from the cached copy. 2: the same, and a draw that would take the cached copy rebuilds it
// too and counts a different result (the rebuilt key is used).
// sotc: N5: unset with SOTC_NATIVE on, the default is 1; an explicit SOTC_PIPE_DIRTY
// (0, 1 or 2) always wins.
static int PipeDirtyMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_PIPE_DIRTY");
        if (!v) {
            return SotcNative::On() ? 1 : 0;
        }
        return v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

namespace {
// sotc: counters of SOTC_PIPE_DIRTY, printed by ReportPipeDirty.
struct PipeDirtyStats {
    std::atomic<u64> key_rebuilt{};    ///< register parts built (mode 1/2)
    std::atomic<u64> key_skipped{};    ///< register parts taken from the cached copy
    std::atomic<u64> mismatch_key{};   ///< =2: the cached register part differed from a rebuild
    std::atomic<u64> flat_refresh{};   ///< flat buffer refreshes of an existing program
    std::atomic<u64> flat_unchanged{}; ///< of those, the words after the refresh were the same
    std::atomic<u64> flat_words{};     ///< flat buffer words refreshed (sum)
};
PipeDirtyStats g_pipe_dirty;
} // namespace

// sotc: each register member is copied whole, so its bytes equal the registers' own bytes and a
// byte compare of the members is exact.
static_assert(std::is_trivially_copyable_v<PipeKeyRegs>);

template <typename T>
static bool SameBytes(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

static bool PipeKeyRegsMatch(const AmdGpu::Regs& regs, const PipeKeyRegs& s) {
    return SameBytes(regs.depth_buffer, s.depth_buffer) &&
           SameBytes(regs.depth_render_override, s.depth_render_override) &&
           SameBytes(regs.clipper_control, s.clipper_control) &&
           SameBytes(regs.polygon_control, s.polygon_control) &&
           SameBytes(regs.primitive_type, s.primitive_type) &&
           SameBytes(regs.stage_enable, s.stage_enable) &&
           SameBytes(regs.ls_hs_config, s.ls_hs_config) &&
           SameBytes(regs.color_control, s.color_control) &&
           SameBytes(regs.color_shader_mask, s.color_shader_mask) &&
           SameBytes(regs.color_target_mask, s.color_target_mask) &&
           SameBytes(regs.color_export_format, s.color_export_format) &&
           std::memcmp(regs.color_buffers, s.color_buffers.data(), sizeof(s.color_buffers)) == 0 &&
           SameBytes(regs.blend_control, s.blend_control);
}

static void PipeKeyRegsCapture(const AmdGpu::Regs& regs, PipeKeyRegs& s) {
    std::memcpy(&s.depth_buffer, &regs.depth_buffer, sizeof(s.depth_buffer));
    std::memcpy(&s.depth_render_override, &regs.depth_render_override,
                sizeof(s.depth_render_override));
    std::memcpy(&s.clipper_control, &regs.clipper_control, sizeof(s.clipper_control));
    std::memcpy(&s.polygon_control, &regs.polygon_control, sizeof(s.polygon_control));
    std::memcpy(&s.primitive_type, &regs.primitive_type, sizeof(s.primitive_type));
    std::memcpy(&s.stage_enable, &regs.stage_enable, sizeof(s.stage_enable));
    std::memcpy(&s.ls_hs_config, &regs.ls_hs_config, sizeof(s.ls_hs_config));
    std::memcpy(&s.color_control, &regs.color_control, sizeof(s.color_control));
    std::memcpy(&s.color_shader_mask, &regs.color_shader_mask, sizeof(s.color_shader_mask));
    std::memcpy(&s.color_target_mask, &regs.color_target_mask, sizeof(s.color_target_mask));
    std::memcpy(&s.color_export_format, &regs.color_export_format,
                sizeof(s.color_export_format));
    std::memcpy(s.color_buffers.data(), regs.color_buffers, sizeof(s.color_buffers));
    std::memcpy(&s.blend_control, &regs.blend_control, sizeof(s.blend_control));
}

// sotc: writes a report line for the 5 s report: counts since the previous call, for
// SOTC_PIPE_DIRTY=1/2. Prints nothing while the switch is off or the window had no work. One
// thread only.
void ReportPipeDirty(std::string& out, double seconds) {
    if (PipeDirtyMode() == 0) {
        return;
    }
    const std::array<const std::atomic<u64>*, 6> counters{
        &g_pipe_dirty.key_rebuilt,   &g_pipe_dirty.key_skipped,   &g_pipe_dirty.mismatch_key,
        &g_pipe_dirty.flat_refresh,  &g_pipe_dirty.flat_unchanged, &g_pipe_dirty.flat_words,
    };
    static std::array<u64, counters.size()> prev{};
    std::array<u64, counters.size()> d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        const u64 now = counters[i]->load(std::memory_order_relaxed);
        d[i] = now - prev[i];
        prev[i] = now;
    }
    const u64 keys = d[0] + d[1];
    if (keys == 0 && d[3] == 0) {
        return;
    }
    const auto pct = [](u64 part, u64 whole) {
        return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
    };
    char line[480];
    std::snprintf(
        line, sizeof(line),
        "SOTC pipe dirty: mode %d, graphics key register part rebuilt %llu, skipped %llu "
        "(%.0f%%), mismatches %llu; flat buf refreshed %llu, unchanged %llu (%.0f%%), %.0f words "
        "each, %.1f refreshes/s\n",
        PipeDirtyMode(), static_cast<unsigned long long>(d[0]),
        static_cast<unsigned long long>(d[1]), pct(d[1], keys),
        static_cast<unsigned long long>(d[2]), static_cast<unsigned long long>(d[3]),
        static_cast<unsigned long long>(d[4]), pct(d[4], d[3]),
        d[3] ? static_cast<double>(d[5]) / static_cast<double>(d[3]) : 0.0,
        seconds > 0.0 ? static_cast<double>(d[3]) / seconds : 0.0);
    out += line;
}

// sotc: SOTC_PIPE_FAST. Unset or 0: the lookups run in full, as before. 1: a draw whose recorded
// inputs are unchanged takes the recorded pipeline and stage state. 2: the check runs, the full
// lookup runs too, and a difference is counted (the full lookup's result is used).
static int PipeFastMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_PIPE_FAST");
        return v && v[0] == '1' ? 1 : v && v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

namespace {
// sotc: counters of SOTC_PIPE_FAST, printed by ReportPipeFast. The fields are in the order of
// the array there.
struct PipeFastStats {
    std::atomic<u64> gfx_hits{};
    std::atomic<u64> gfx_miss_record{};
    std::atomic<u64> gfx_miss_async{};
    std::atomic<u64> gfx_miss_regs{};
    std::atomic<u64> gfx_miss_params{};
    std::atomic<u64> gfx_miss_sharp{};
    std::atomic<u64> gfx_miss_runtime{};
    std::atomic<u64> cs_hits{};
    std::atomic<u64> cs_miss_record{};
    std::atomic<u64> cs_miss_params{};
    std::atomic<u64> cs_miss_sharp{};
    std::atomic<u64> cs_miss_runtime{};
    std::atomic<u64> mismatch_gfx{};
    std::atomic<u64> mismatch_cs{};
    std::atomic<u64> check_ns{}; ///< time spent in the checks (hits and misses)
};
PipeFastStats g_pipe_fast;

// sotc: counts one miss of a check, by its reason.
void CountFastMiss(bool graphics, PipeFastMiss miss) {
    auto& s = g_pipe_fast;
    if (graphics) {
        switch (miss) {
        case PipeFastMiss::Record: s.gfx_miss_record.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Async: s.gfx_miss_async.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Regs: s.gfx_miss_regs.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Params: s.gfx_miss_params.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Sharp: s.gfx_miss_sharp.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Runtime: s.gfx_miss_runtime.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::None: break;
        }
    } else {
        switch (miss) {
        case PipeFastMiss::Record: s.cs_miss_record.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Params: s.cs_miss_params.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Sharp: s.cs_miss_sharp.fetch_add(1, std::memory_order_relaxed); break;
        case PipeFastMiss::Runtime: s.cs_miss_runtime.fetch_add(1, std::memory_order_relaxed); break;
        default: break;
        }
    }
}
} // namespace

// sotc: SOTC_PIPE_FAST. The stages RefreshGraphicsStages binds for the stage selection in regs, in
// its order (the fragment stage first). Keep in step with the switch of that function.
static size_t PipeFastStageList(const AmdGpu::Regs& regs,
                                std::array<std::pair<Shader::HwStage, Shader::SwStage>, 5>& out) {
    using Hw = Shader::HwStage;
    using Sw = Shader::SwStage;
    size_t n = 0;
    out[n++] = {Hw::Fragment, Sw::Fragment};
    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        out[n++] = {Hw::Vertex, Sw::Vertex};
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        out[n++] = {Hw::Export, Sw::Vertex};
        out[n++] = {Hw::Geometry, Sw::Geometry};
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        out[n++] = {Hw::Hull, Sw::TessellationControl};
        out[n++] = {Hw::Vertex, Sw::TessellationEval};
        out[n++] = {Hw::Local, Sw::Vertex};
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        out[n++] = {Hw::Hull, Sw::TessellationControl};
        out[n++] = {Hw::Export, Sw::TessellationEval};
        out[n++] = {Hw::Local, Sw::Vertex};
        out[n++] = {Hw::Geometry, Sw::Geometry};
        break;
    default:
        break;
    }
    return n;
}

// sotc: SOTC_PIPE_FAST. Fills the record of one stage the full lookup bound, from its program. Reads
// the guest memory the stage reads now, after the lookup. False when the stage cannot be checked
// later (its fetch shader is unknown): the record is not kept.
bool PipelineCache::FillFastStage(PipeFastStage& st, Shader::Info& info, Shader::HwStage hw,
                                  Shader::SwStage sw, const Shader::ShaderParams& params,
                                  vk::ShaderModule module, Shader::Gcn::FetchShaderData* fetch) {
    st.absent = false;
    st.hw_stage = hw;
    st.sw_stage = sw;
    st.info = &info;
    st.module = module;
    st.hash = params.hash;
    st.pgm_base = params.Base();
    std::memcpy(st.user_data.data(), params.user_data.data(), sizeof(st.user_data));
    st.flat.assign(info.FlatBuf().begin(), info.FlatBuf().end());
    st.runtime_info = runtime_infos[static_cast<u32>(sw)];
    // The same tests as the specialization and GetProgram make.
    st.has_tess = info.sw_stage == Shader::SwStage::TessellationControl ||
                  info.sw_stage == Shader::SwStage::TessellationEval;
    if (st.has_tess) {
        info.ReadTessConstantBuffer(st.tess);
    }
    st.has_fetch = info.sw_stage == Shader::SwStage::Vertex && info.has_fetch_shader;
    st.fetch_code.clear();
    if (st.has_fetch) {
        if (!fetch) {
            return false;
        }
        const u8* backing = Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(
            Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base)));
        const size_t words = fetch->size / sizeof(u32);
        if (!backing || words == 0) {
            return false;
        }
        st.fetch_code.resize(words);
        std::memcpy(st.fetch_code.data(), backing, words * sizeof(u32));
    }
    return true;
}

// sotc: SOTC_PIPE_FAST. Checks one recorded stage against the draw. Makes the same updates to the
// program's info that GetProgram makes (PgmBase, UserData, the flat buffer), so the guest memory
// is read as the full lookup reads it. On a match the stage's info and module are set in slot, as
// the full lookup leaves them.
PipeFastMiss PipelineCache::FastVerifyStage(const PipeFastStage& st,
                                            const Shader::ShaderParams& params, size_t slot) {
    if (params.hash != st.hash || params.Base() != st.pgm_base ||
        std::memcmp(params.user_data.data(), st.user_data.data(), sizeof(st.user_data)) != 0) {
        return PipeFastMiss::Params;
    }
    Shader::Info& info = *st.info;
    info.PgmBase() = params.Base();
    info.UserData() = params.user_data;
    info.RefreshFlatBuf();
    if (!std::ranges::equal(info.FlatBuf(), st.flat)) {
        return PipeFastMiss::Sharp;
    }
    if (st.has_tess) {
        Shader::TessellationDataConstantBuffer tess{};
        info.ReadTessConstantBuffer(tess);
        if (std::memcmp(&tess, &st.tess, sizeof(tess)) != 0) {
            return PipeFastMiss::Sharp;
        }
    }
    if (st.has_fetch) {
        const u8* backing = Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(
            Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base)));
        if (!backing || std::memcmp(backing, st.fetch_code.data(),
                                    st.fetch_code.size() * sizeof(u32)) != 0) {
            return PipeFastMiss::Sharp;
        }
    }
    infos[slot] = &info;
    modules[slot] = st.module;
    // BuildRuntimeInfo reads the registers, the draw's indirect params and (for the local stage)
    // the hull stage's constants: compared as the specialization receives it.
    const auto& rt = BuildRuntimeInfo(st.hw_stage, st.sw_stage);
    return rt == st.runtime_info ? PipeFastMiss::None : PipeFastMiss::Runtime;
}

// sotc: SOTC_PIPE_FAST. The check of a graphics draw against the record. Each stage of the record
// is verified in bind order (the hull stage before the local one, which reads its constants).
bool PipelineCache::GraphicsFastCheck(PipeFastState& out) {
    const auto& rec = pipe_fast_gfx;
    if (!rec.valid || !last_graphics_pipeline || last_graphics_pipeline != rec.pipeline ||
        last_graphics_size != graphics_pipelines.size()) {
        CountFastMiss(true, PipeFastMiss::Record);
        return false;
    }
    // The full lookup finishes the translations that are done first.
    if (async_done_count.load(std::memory_order_acquire) != 0) {
        CountFastMiss(true, PipeFastMiss::Async);
        return false;
    }
    const auto& regs = liverpool->regs;
    if (!PipeKeyRegsMatch(regs, rec.regs.key) ||
        !SameBytes(regs.vgt_gs_mode, rec.regs.vgt_gs_mode) ||
        !SameBytes(regs.vgt_strmout_config, rec.regs.vgt_strmout_config)) {
        CountFastMiss(true, PipeFastMiss::Regs);
        return false;
    }
    // The same start as RefreshGraphicsStages.
    infos.fill(nullptr);
    modules.fill(nullptr);
    for (const auto& st : rec.stages) {
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(st.hw_stage));
        const bool has_pgm = pgm && pgm->Address<u32*>();
        PipeFastMiss miss = PipeFastMiss::None;
        if (st.absent) {
            if (has_pgm) { // a program where the record had none: the full lookup binds it
                miss = PipeFastMiss::Params;
            }
        } else if (!has_pgm) {
            miss = PipeFastMiss::Params;
        } else {
            miss = FastVerifyStage(st, AmdGpu::GetParams(*pgm), static_cast<size_t>(st.sw_stage));
        }
        if (miss != PipeFastMiss::None) {
            CountFastMiss(true, miss);
            return false;
        }
    }
    out.pipeline = rec.pipeline;
    out.infos = infos;
    out.modules = modules;
    out.fetch = rec.fetch;
    return true;
}

// sotc: SOTC_PIPE_FAST. The check of a compute dispatch against the record.
bool PipelineCache::ComputeFastCheck(PipeFastState& out) {
    const auto& rec = pipe_fast_cs;
    if (!rec.valid || !rec.pipeline || rec.pipelines_size != compute_pipelines.size()) {
        CountFastMiss(false, PipeFastMiss::Record);
        return false;
    }
    const auto params = AmdGpu::GetParams(liverpool->GetCsRegs());
    // A shader still on a worker is waited for by the full lookup.
    if (compute_prefetch && pending_programs.contains(params.hash)) {
        CountFastMiss(false, PipeFastMiss::Record);
        return false;
    }
    const PipeFastMiss miss = FastVerifyStage(rec.stage, params, 0);
    if (miss != PipeFastMiss::None) {
        CountFastMiss(false, miss);
        return false;
    }
    out.compute = rec.pipeline;
    out.infos[0] = infos[0];
    out.modules[0] = modules[0];
    return true;
}

// sotc: SOTC_PIPE_FAST. Records the state a full graphics lookup left, when it returned pipeline.
// Only the stages it bound are recorded (the stage selection is in the record's registers).
void PipelineCache::RecordGraphicsFast(const GraphicsPipeline* pipeline) {
    auto& rec = pipe_fast_gfx;
    rec.valid = false;
    // sotc: SOTC_ADDR_FALLBACK: a fallback lookup is not recorded, or its hits would keep the descriptor pipeline after
    // the table one is built (the map size does not move when a queued entry is filled).
    if (&pipeline->GetProfile() == &desc_profile) {
        return;
    }
    const auto& regs = liverpool->regs;
    std::array<std::pair<Shader::HwStage, Shader::SwStage>, 5> list{};
    const size_t count = PipeFastStageList(regs, list);
    rec.stages.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const auto [hw, sw] = list[i];
        const auto slot = static_cast<size_t>(static_cast<u32>(sw));
        PipeFastStage& st = rec.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(hw));
        if (!pgm || !pgm->Address<u32*>()) {
            if (infos[slot]) { // bound without a program: not a state this record can check
                return;
            }
            st.absent = true;
            st.hw_stage = hw;
            st.sw_stage = sw;
            continue;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        const auto it = program_cache.find(params.hash);
        if (it == program_cache.end() || infos[slot] != &it.value()->info) {
            return;
        }
        if (!FillFastStage(st, it.value()->info, hw, sw, params, modules[slot], fetch_shader)) {
            return;
        }
    }
    PipeKeyRegsCapture(regs, rec.regs.key);
    std::memcpy(&rec.regs.vgt_gs_mode, &regs.vgt_gs_mode, sizeof(rec.regs.vgt_gs_mode));
    std::memcpy(&rec.regs.vgt_strmout_config, &regs.vgt_strmout_config,
                sizeof(rec.regs.vgt_strmout_config));
    rec.fetch = fetch_shader;
    rec.pipeline = pipeline;
    rec.pipelines_size = graphics_pipelines.size();
    rec.valid = true;
}

// sotc: SOTC_PIPE_FAST. Records the state a full compute lookup left, when it returned pipeline.
void PipelineCache::RecordComputeFast(const ComputePipeline* pipeline) {
    auto& rec = pipe_fast_cs;
    rec.valid = false;
    if (&pipeline->GetProfile() == &desc_profile) { // sotc: SOTC_ADDR_FALLBACK, as RecordGraphicsFast
        return;
    }
    const auto params = AmdGpu::GetParams(liverpool->GetCsRegs());
    const auto it = program_cache.find(params.hash);
    if (it == program_cache.end() || infos[0] != &it.value()->info) {
        return;
    }
    if (!FillFastStage(rec.stage, it.value()->info, Shader::HwStage::Compute,
                       Shader::SwStage::Compute, params, modules[0], nullptr)) {
        return;
    }
    rec.key = compute_key;
    rec.pipeline = pipeline;
    rec.pipelines_size = compute_pipelines.size();
    rec.valid = true;
}

// sotc: SOTC_PIPE_FAST. The graphics lookup: the check first; a hit returns the recorded pipeline
// (mode 1). Otherwise the full lookup runs, and a hit is compared with its result (mode 2).
const GraphicsPipeline* PipelineCache::GetGraphicsPipelineLookup(const DrawIndirectParams params) {
    const int fast_mode = PipeFastMode();
    if (fast_mode == 0) {
        return NoteKeyMemo(GetGraphicsPipelineLookupSlow(params), params);
    }
    // The full lookup sets these too; the check needs them first (BuildRuntimeInfo reads them).
    draw_indirect_params = params;
    translation_pending = false;
    PipeFastState cand{};
    const auto check_start = std::chrono::steady_clock::now();
    const bool hit = GraphicsFastCheck(cand);
    g_pipe_fast.check_ns.fetch_add(
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - check_start)
                             .count()),
        std::memory_order_relaxed);
    if (hit && fast_mode == 1) {
        fetch_shader = cand.fetch;
        std::memcpy(&graphics_key, &last_graphics_key, sizeof(graphics_key));
        g_pipe_fast.gfx_hits.fetch_add(1, std::memory_order_relaxed);
        if (cand.pipeline->IsLinked()) {
            g_gpl_counters.linked_served.fetch_add(1, std::memory_order_relaxed);
        }
        return NoteKeyMemo(cand.pipeline, params);
    }
    pipe_fast_gfx.valid = false;
    const GraphicsPipeline* result = GetGraphicsPipelineLookupSlow(params);
    if (hit) { // =2: the check's state against the full lookup's; the full result is used
        g_pipe_fast.gfx_hits.fetch_add(1, std::memory_order_relaxed);
        if (result != cand.pipeline || cand.fetch != fetch_shader || cand.infos != infos ||
            cand.modules != modules) {
            g_pipe_fast.mismatch_gfx.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (result) {
        RecordGraphicsFast(result);
    }
    return NoteKeyMemo(result, params);
}

// sotc: SOTC_PIPE_FAST. The compute lookup: the check first, then as GetGraphicsPipelineLookup.
const ComputePipeline* PipelineCache::GetComputePipelineLookup() {
    const int fast_mode = PipeFastMode();
    if (fast_mode == 0) {
        return GetComputePipelineLookupSlow();
    }
    PipeFastState cand{};
    const auto check_start = std::chrono::steady_clock::now();
    const bool hit = ComputeFastCheck(cand);
    g_pipe_fast.check_ns.fetch_add(
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - check_start)
                             .count()),
        std::memory_order_relaxed);
    if (hit && fast_mode == 1) {
        compute_key = pipe_fast_cs.key;
        g_pipe_fast.cs_hits.fetch_add(1, std::memory_order_relaxed);
        return cand.compute;
    }
    pipe_fast_cs.valid = false;
    const ComputePipeline* result = GetComputePipelineLookupSlow();
    if (hit) { // =2: as for graphics
        g_pipe_fast.cs_hits.fetch_add(1, std::memory_order_relaxed);
        if (result != cand.compute || cand.infos[0] != infos[0] || cand.modules[0] != modules[0]) {
            g_pipe_fast.mismatch_cs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (result) {
        RecordComputeFast(result);
    }
    return result;
}

// sotc: writes a report line for the 5 s report: counts since the previous call, for SOTC_PIPE_FAST.
// Prints nothing while the switch is off or the window had no lookups. One thread only.
void ReportPipeFast(std::string& out, double seconds) {
    const int mode = PipeFastMode();
    if (mode == 0) {
        return;
    }
    // Same order as the fields of PipeFastStats.
    const std::array<const std::atomic<u64>*, 15> counters{
        &g_pipe_fast.gfx_hits,        &g_pipe_fast.gfx_miss_record, &g_pipe_fast.gfx_miss_async,
        &g_pipe_fast.gfx_miss_regs,   &g_pipe_fast.gfx_miss_params, &g_pipe_fast.gfx_miss_sharp,
        &g_pipe_fast.gfx_miss_runtime, &g_pipe_fast.cs_hits,        &g_pipe_fast.cs_miss_record,
        &g_pipe_fast.cs_miss_params,  &g_pipe_fast.cs_miss_sharp,   &g_pipe_fast.cs_miss_runtime,
        &g_pipe_fast.mismatch_gfx,    &g_pipe_fast.mismatch_cs,     &g_pipe_fast.check_ns,
    };
    static std::array<u64, counters.size()> prev{};
    std::array<u64, counters.size()> d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        const u64 now = counters[i]->load(std::memory_order_relaxed);
        d[i] = now - prev[i];
        prev[i] = now;
    }
    // Indices as in the array above.
    constexpr std::size_t kGfxHits = 0, kGfxRecord = 1, kGfxAsync = 2, kGfxRegs = 3,
                          kGfxParams = 4, kGfxSharp = 5, kGfxRuntime = 6, kCsHits = 7,
                          kCsRecord = 8, kCsParams = 9, kCsSharp = 10, kCsRuntime = 11,
                          kMismatchGfx = 12, kMismatchCs = 13, kCheckNs = 14;
    const u64 gfx_miss = d[kGfxRecord] + d[kGfxAsync] + d[kGfxRegs] + d[kGfxParams] +
                         d[kGfxSharp] + d[kGfxRuntime];
    const u64 cs_miss = d[kCsRecord] + d[kCsParams] + d[kCsSharp] + d[kCsRuntime];
    const u64 gfx_total = d[kGfxHits] + gfx_miss;
    const u64 cs_total = d[kCsHits] + cs_miss;
    if (gfx_total == 0 && cs_total == 0) {
        return;
    }
    const auto pct = [](u64 part, u64 whole) {
        return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
    };
    const auto U = [](u64 v) { return static_cast<unsigned long long>(v); };
    char line[640];
    std::snprintf(
        line, sizeof(line),
        "SOTC pipe fast: mode %d, graphics %llu/%llu hits (%.0f%%), misses: record %llu, async "
        "%llu, regs %llu, params %llu, sharp %llu, runtime %llu; compute %llu/%llu hits (%.0f%%), "
        "misses: record %llu, params %llu, sharp %llu, runtime %llu; mismatches %llu (graphics "
        "%llu, compute %llu); checks %.2f ms/s\n",
        mode, U(d[kGfxHits]), U(gfx_total), pct(d[kGfxHits], gfx_total), U(d[kGfxRecord]),
        U(d[kGfxAsync]), U(d[kGfxRegs]), U(d[kGfxParams]), U(d[kGfxSharp]), U(d[kGfxRuntime]),
        U(d[kCsHits]), U(cs_total), pct(d[kCsHits], cs_total), U(d[kCsRecord]), U(d[kCsParams]),
        U(d[kCsSharp]), U(d[kCsRuntime]), U(d[kMismatchGfx] + d[kMismatchCs]),
        U(d[kMismatchGfx]), U(d[kMismatchCs]),
        seconds > 0.0 ? static_cast<double>(d[kCheckNs]) / 1e6 / seconds : 0.0);
    out += line;
}

// sotc: the register part of the graphics key: everything RefreshGraphicsKey derives from the
// registers before the shader stages are bound. Reads no guest memory and no shader state.
void PipelineCache::RefreshGraphicsKeyRegs(GraphicsPipelineKey& key) const {
    const auto& regs = liverpool->regs;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }
}

// sotc: SOTC_PIPE_DIRTY=1/2: fills the register part of graphics_key. The cached copy is used
// while PipeKeyRegs matches the registers; a rebuild refreshes both the copy and the snapshot.
void PipelineCache::RefreshGraphicsKeyRegsDirty(int mode) {
    const auto& regs = liverpool->regs;
    if (pipe_dirty_valid && PipeKeyRegsMatch(regs, pipe_dirty_regs)) {
        std::memcpy(&graphics_key, &pipe_dirty_key, sizeof(GraphicsPipelineKey));
        g_pipe_dirty.key_skipped.fetch_add(1, std::memory_order_relaxed);
        if (mode == 2) {
            GraphicsPipelineKey fresh{};
            RefreshGraphicsKeyRegs(fresh);
            if (std::memcmp(&fresh, &graphics_key, sizeof(GraphicsPipelineKey)) != 0) {
                g_pipe_dirty.mismatch_key.fetch_add(1, std::memory_order_relaxed);
                std::memcpy(&graphics_key, &fresh, sizeof(GraphicsPipelineKey));
                std::memcpy(&pipe_dirty_key, &fresh, sizeof(GraphicsPipelineKey));
            }
        }
        return;
    }
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    RefreshGraphicsKeyRegs(graphics_key);
    g_pipe_dirty.key_rebuilt.fetch_add(1, std::memory_order_relaxed);
    PipeKeyRegsCapture(regs, pipe_dirty_regs);
    std::memcpy(&pipe_dirty_key, &graphics_key, sizeof(GraphicsPipelineKey));
    pipe_dirty_valid = true;
}

bool PipelineCache::RefreshGraphicsKey() {
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    if (const int dirty_mode = PipeDirtyMode(); dirty_mode != 0) {
        RefreshGraphicsKeyRegsDirty(dirty_mode);
    } else {
        std::memset(&key, 0, sizeof(GraphicsPipelineKey));
        RefreshGraphicsKeyRegs(key);
    }
    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();
    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages() {
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    fetch_shader = nullptr;

    Shader::Backend::Bindings binding{};
    // sotc: SOTC_HEAP stage variants (ShaderParams::variant). The fragment is the heap stage of the pipeline when it
    // qualifies (fragment_variant); the other stages are then plain (other_variant). Both default unless the switch is on.
    // sotc: SOTC_SHADER_ADDR: a stage that is not the heap stage is plain too (non_heap_variant). Slot words and shader
    // addresses cannot share the push bytes (the packet is 56..128), and a bindless stage never takes addresses
    // (ShaderAddrBuffer), so the stages that would have slot words take descriptors instead and their buffers can be
    // addressed. Without the switch this is the default, as before.
    // sotc: SOTC_ADDR_FALLBACK: a fallback lookup binds the descriptor-mode stages (default variant, no heap stage).
    const u8 non_heap_variant = profile.shader_addr != 0 && !addr_fallback_draw ? Shader::StageVariantPlain
                                                                                : Shader::StageVariantDefault;
    const bool heap_on = profile.heap_images && profile.support_int64 && !addr_fallback_draw;
    u8 fragment_variant = non_heap_variant;
    u8 other_variant = non_heap_variant;
    // sotc: SOTC_HEAP_VS: the vertex stage's variant (bind_vertex below). Equal to other_variant with the switch off.
    u8 vertex_variant = non_heap_variant;
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        // sotc: SOTC_STAGE_BINDINGS: the descriptors of this stage are numbered from 0 in its own
        // set. The buffer count is not reset: it indexes the push constant offsets, which all
        // the stages share.
        if (profile.stage_bindings) {
            binding.unified = 0;
        }
        if (stage_out == SwStage::Vertex) { // sotc: SOTC_VS_BINDING_BASE
            binding = Shader::Backend::VsStartBindings(binding, profile.stage_bindings);
        }
        // sotc: the bindings of the later stages follow from this one's: they wait for it.
        if (translation_pending || !regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        auto params = AmdGpu::GetParams(*pgm);
        // sotc: SOTC_VS_DRAW_KIND (info.h): an indirect draw of a shared instanced program sees user data 0 in the
        // start instance SGPR (the shader adds BaseInstance). On the GPU the command processor overwrites that SGPR,
        // so its user data is never the program's input.
        if (stage_out == SwStage::Vertex && draw_indirect_params.instance_sgpr_offset != 0 &&
            VsDrawKindShared(stage_in, pgm->settings.vgpr_comp_cnt, draw_indirect_params.vertex_sgpr_offset,
                             draw_indirect_params.instance_sgpr_offset)) {
            std::copy(params.user_data.begin(), params.user_data.end(), vs_draw_kind_user_data.begin());
            vs_draw_kind_user_data[Shader::kVsDrawKindInstanceSgpr - 0x4c] = 0;
            params.user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{vs_draw_kind_user_data};
        }
        // sotc: SOTC_HEAP: a variant is a program of its own, so its hash differs (the program, memo and key caches).
        params.variant = stage_out == SwStage::Fragment  ? fragment_variant
                         : stage_out == SwStage::Vertex ? vertex_variant
                                                        : other_variant;
        if (params.variant != Shader::StageVariantDefault) {
            params.guest_hash = params.hash; // sotc: Info::pgm_hash stays the game's hash
            params.hash = HashCombine(params.hash, params.variant);
        }
        // sotc: SOTC_HEAP: a candidate refused before (this session or a saved one) is not translated again; the
        // caller binds the stage's other variant, as for a refusal.
        if (params.variant == Shader::StageVariantHeap && heap_refused.contains(params.hash)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            modules[stage_out_idx] = nullptr;
            return false;
        }
        std::tie(infos[stage_out_idx], modules[stage_out_idx], key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding, async_translate_ok);
        return !translation_pending;
    };

    infos.fill(nullptr);
    modules.fill(nullptr);

    // sotc: SOTC_HEAP: the fragment is bound as the heap candidate. It stays the heap stage when it passes the
    // recompiler's static rules and the host's (Heap::StageRefused), and then the other stages are plain. A refused
    // fragment is bound again as today, and the pipeline keeps the bindless path of its other stages.
    const auto binding_before_fragment = binding;
    if (heap_on) {
        fragment_variant = Shader::StageVariantHeap;
        bind_stage(HwStage::Fragment, SwStage::Fragment);
        const auto* heap_fs = infos[static_cast<u32>(SwStage::Fragment)];
        if (heap_fs && !SotcArch::Heap::StageRefused(*heap_fs)) {
            other_variant = Shader::StageVariantPlain;
        } else if (heap_fs || !translation_pending) {
            if (heap_fs) {
                NoteHeapRefused(heap_fs->CacheHash());
            }
            fragment_variant = non_heap_variant;
            binding = binding_before_fragment;
            bind_stage(HwStage::Fragment, SwStage::Fragment);
        }
    } else {
        bind_stage(HwStage::Fragment, SwStage::Fragment);
    }

    const auto* fs_info = infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    // sotc: SOTC_HEAP_VS. The vertex stage is bound as a heap candidate, after the stages bound before it (the fragment,
    // and in tessellation the hull and eval stages). It stays heap when the static and host rules pass
    // (Heap::StageRefused). A packet pipeline has no slot words, so the heap vertex needs every other stage plain:
    // with a heap fragment they are plain already; without one, the fragment is bound again as plain when it is the
    // only stage before the vertex, and a hull or eval stage with slot words refuses the vertex. The stages bound after
    // the vertex take the plain variant once it is heap. Switch off: the vertex is bound as the others, as before.
    const bool heap_vs_on = heap_on && SotcArch::Heap::VertexOn();
    const auto bind_vertex = [&](HwStage stage_in, bool fragment_only_before) -> bool {
        if (!heap_vs_on) {
            vertex_variant = other_variant;
            return bind_stage(stage_in, SwStage::Vertex);
        }
        // Slot words: a default (descriptor and slot-word) stage that binds bindless resources.
        const auto slot_words = [](const Shader::Info* info) {
            return info && info->variant == Shader::StageVariantDefault &&
                   (Shader::UsesBindlessSamplers(*info) || Shader::UsesBindlessImages(*info));
        };
        // The fragment is the only stage before the vertex here: it is bound again as plain when it has slot words.
        if (fragment_only_before && fragment_variant != Shader::StageVariantHeap &&
            slot_words(infos[static_cast<u32>(SwStage::Fragment)])) {
            fragment_variant = Shader::StageVariantPlain;
            binding = binding_before_fragment;
            bind_stage(HwStage::Fragment, SwStage::Fragment);
            if (translation_pending) {
                return false;
            }
        }
        if (slot_words(infos[static_cast<u32>(SwStage::Fragment)]) ||
            slot_words(infos[static_cast<u32>(SwStage::TessellationControl)]) ||
            slot_words(infos[static_cast<u32>(SwStage::TessellationEval)])) {
            vertex_variant = other_variant;
            return bind_stage(stage_in, SwStage::Vertex);
        }
        const auto binding_before_vertex = binding;
        vertex_variant = Shader::StageVariantHeap;
        bind_stage(stage_in, SwStage::Vertex);
        const auto* heap_vs = infos[static_cast<u32>(SwStage::Vertex)];
        if (heap_vs && !SotcArch::Heap::StageRefused(*heap_vs)) {
            other_variant = Shader::StageVariantPlain;
            return !translation_pending;
        }
        if (heap_vs || !translation_pending) {
            if (heap_vs) {
                NoteHeapRefused(heap_vs->CacheHash());
            }
            vertex_variant = other_variant;
            binding = binding_before_vertex;
            return bind_stage(stage_in, SwStage::Vertex);
        }
        return false;
    };

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_vertex(HwStage::Export, true)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_vertex(HwStage::Local, false)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_vertex(HwStage::Local, false)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_vertex(HwStage::Vertex, true);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    if (translation_pending) {
        return false;
    }

    const auto* vs_info = infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

void PipelineCache::NoteHeapRefused(u64 candidate_hash) {
    if (!heap_refused.insert(candidate_hash).second) {
        return;
    }
    // sotc: the whole list is written again (a few KiB): the session may end at any time (launcher Stop, _Exit).
    std::vector<u8> blob(heap_refused.size() * sizeof(u64));
    size_t offset = 0;
    for (const u64 hash : heap_refused) {
        std::memcpy(blob.data() + offset, &hash, sizeof(hash));
        offset += sizeof(hash);
    }
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "heap_refused", std::move(blob));
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    auto cs_params = AmdGpu::GetParams(cs_pgm);
    // sotc: SOTC_HEAP: the compute stage is the heap stage when it qualifies (see RefreshGraphicsStages); otherwise
    // it is compiled as today.
    bool compute_bound = false;
    // sotc: SOTC_ADDR_FALLBACK: the table pass queues a missing translation on a worker (the dispatch then takes the
    // descriptor-mode stage, translated as a descriptor-mode run translates it); the fallback pass binds that stage.
    const bool table_async = AddrFallbackComputeOn() && !addr_fallback_draw;
    if (profile.heap_images && profile.support_int64 && !addr_fallback_draw) {
        auto heap_params = cs_params;
        heap_params.variant = Shader::StageVariantHeap;
        heap_params.guest_hash = cs_params.hash;
        heap_params.hash = HashCombine(cs_params.hash, heap_params.variant);
        // sotc: a candidate refused before (heap_refused) is not translated again.
        if (!heap_refused.contains(heap_params.hash)) {
            std::tie(infos[0], modules[0], compute_key.value) =
                GetProgram(HwStage::Compute, SwStage::Compute, heap_params, binding, table_async);
            // A refused candidate is compiled again as today; a pending one answers on a later call.
            compute_bound = !infos[0] || !SotcArch::Heap::StageRefused(*infos[0]);
            if (!compute_bound) {
                NoteHeapRefused(infos[0]->CacheHash());
                binding = {};
            } else if (!pre_compute_pred.empty()) {
                NoteComputeDispatch(cs_pgm, cs_params.hash); // sotc: SOTC_HEAP, the heap prebuild's verify (once)
            }
        }
    }
    if (!compute_bound) {
        // sotc: SOTC_SHADER_ADDR: without the heap the compute stage is plain, as the graphics stages are (see
        // RefreshGraphicsStages). Its variant is mixed into the hash like the heap variant above.
        auto plain_params = cs_params;
        if (profile.shader_addr != 0 && !addr_fallback_draw) {
            plain_params.variant = Shader::StageVariantPlain;
            plain_params.guest_hash = cs_params.hash;
            plain_params.hash = HashCombine(cs_params.hash, plain_params.variant);
        }
        std::tie(infos[0], modules[0], compute_key.value) =
            GetProgram(HwStage::Compute, SwStage::Compute, plain_params, binding, table_async);
        if (!pre_compute_pred.empty()) {
            NoteComputeDispatch(cs_pgm, cs_params.hash); // sotc: SOTC_PRETRANSLATE_COMPUTE (verify, once)
        }
    }
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding,
                                              Shader::Pools* worker_pools,
                                              std::vector<u32>* spv_out) {
    // sotc: N5. A shader translated on the command processor is a stall (not on a worker).
    const N5SyncScope n5_scope{N5Kind::Shader, N5Clock::now(), !worker_pools};
    std::optional<SotcBlockLedger::Scope> ledger; // sotc: SOTC_BLOCK_LEDGER, inline translations only
    if (!worker_pools) {
        ledger.emplace(SotcBlockLedger::Translate);
    }
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    // sotc: the frame columns are command-processor time; worker time is counted apart.
    std::optional<SotcStats::FrameTimer> frame_timer;
    if (!worker_pools) {
        frame_timer.emplace(SotcStats::FrameCompile);
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
    }
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    const SotcStats::ScopedTimer stall_timer{SotcStats::shader_ns};
    SotcStats::shaders_compiled.fetch_add(1, std::memory_order_relaxed);
    SotcFrameMetrics::AddCurrent(SotcFrameMetrics::ShaderCompiles, 1); // sotc: SOTC_FRAME_METRICS

    auto phase_start = std::chrono::steady_clock::now();
    const auto phase = [&](int i) {
        if (SotcStats::kFpsLog) {
            const auto now = std::chrono::steady_clock::now();
            SotcStats::compile_phase_ns[i].fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - phase_start).count(),
                std::memory_order_relaxed);
            phase_start = now;
        }
    };
    const auto ir_program = Shader::TranslateProgram(code, worker_pools ? *worker_pools : pools,
                                                     info, runtime_info, StageProfile(info.variant));
    phase(0);
    // sotc: SOTC_ADDR_FALLBACK: a default-variant stage of table mode is translated as descriptor mode translates it.
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(StageProfile(info.variant), runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");
    phase(1);

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv", spv);
    phase(2);
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }
    phase(3);

    if (spv_out) {
        *spv_out = std::move(spv);
    } else {
        RegisterShaderBinary(std::move(spv), info.CacheHash(), perm_idx); // sotc: the variant's own slot
    }
    phase(4);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

// sotc: SOTC_SPEC_MEMO. Unset or 0: off. 1: the specialization memo answers GetProgram (the stage
// memo of SOTC_PIPE_MEMO is not used with it). 2: the memo and the slow path both run; the slow
// answer is returned and every difference is counted.
static int SpecMemoMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_SPEC_MEMO");
        return v && v[0] == '1' ? 1 : v && v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

namespace {
// sotc: counters of SOTC_SPEC_MEMO, printed by ReportSpecMemo (the order of its counter array).
struct SpecMemoStats {
    std::atomic<u64> lookups{};          ///< GetProgram calls probed (owner thread, mode 1/2)
    std::atomic<u64> hits{};             ///< of those, answered by an entry with an equal key
    std::atomic<u64> stores{};           ///< entries stored from a miss
    std::atomic<u64> store_skipped{};    ///< misses not stored: the constructed spec packed differently
    std::atomic<u64> evictions{};        ///< stores that replaced the least recently used entry
    std::atomic<u64> entries{};          ///< entries filled (a gauge, read in the report)
    std::atomic<u64> probe_ns{};         ///< time of the key pass and the lookup
    std::atomic<u64> mismatch_perm{};    ///< =2: memo permutation index differed from the slow path
    std::atomic<u64> mismatch_module{};  ///< =2: same permutation, other shader module
    std::atomic<u64> mismatch_fetch{};   ///< =2: fetch shader data differed
    std::atomic<u64> mismatch_bindings{}; ///< =2: bindings after the stage differed
};
SpecMemoStats g_spec_memo;

// Scratch of the probe of one GetProgram call: the key words and the parsed fetch shader. The
// store after a miss (same call) reads them.
thread_local std::vector<u32> t_spec_words;
thread_local Shader::Gcn::FetchShaderData t_spec_fetch;
thread_local std::vector<u32> t_spec_check;
// sotc: SOTC_SHADER_ADDR (table mode): set while GetProgram looks a compute stage up again after its permutation was
// translated on a worker (TranslateComputeOnWorker): that second lookup does not queue another one.
thread_local bool t_cs_perm_retry = false;

constexpr u32 kSpecNone = SpecMemoEntry::None;

u64 SpecKeyHash(const std::vector<u32>& words, const Program* program, u64 hash,
                Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                const Shader::Backend::Bindings& start) {
    constexpr u64 kPrime = 0x100000001b3ull;
    u64 h = 0xcbf29ce484222325ull ^ static_cast<u64>(reinterpret_cast<uintptr_t>(program));
    h = (h ^ hash) * kPrime;
    h = (h ^ ((u64(static_cast<u32>(hw_stage)) << 8) | u64(static_cast<u32>(sw_stage)))) * kPrime;
    h = (h ^ start.unified) * kPrime;
    h = (h ^ start.buffer) * kPrime;
    for (const u32 w : words) {
        h = (h ^ w) * kPrime;
    }
    return h ^ (h >> 29);
}

// The entry whose key equals the probe, or kSpecNone. Every part of the key is compared; the hash
// only selects the candidates.
u32 SpecMemoFind(const SpecMemoState& memo, u64 key_hash, const Program& program,
                 Shader::HwStage hw_stage, Shader::SwStage sw_stage, u64 hash,
                 const Shader::Backend::Bindings& start, const Shader::RuntimeInfo& rt,
                 bool has_tess, const Shader::TessellationDataConstantBuffer& tess,
                 const Shader::Gcn::FetchShaderData& fetch, const std::vector<u32>& words) {
    const auto [first, last] = memo.index.equal_range(key_hash);
    for (auto it = first; it != last; ++it) {
        const SpecMemoEntry& e = memo.entries[it->second];
        if (e.program != &program || e.hw_stage != hw_stage || e.sw_stage != sw_stage ||
            e.hash != hash || !(e.start == start) || e.has_tess != has_tess ||
            (has_tess && std::memcmp(&e.tess, &tess, sizeof(tess)) != 0) || !(e.runtime_info == rt) ||
            !(e.fetch == fetch) || e.words != words || e.perm >= program.modules.size()) {
            continue;
        }
        return it->second;
    }
    return kSpecNone;
}

void SpecMemoUnlink(SpecMemoState& memo, u32 i) {
    SpecMemoEntry& e = memo.entries[i];
    if (e.prev != kSpecNone) {
        memo.entries[e.prev].next = e.next;
    } else {
        memo.head = e.next;
    }
    if (e.next != kSpecNone) {
        memo.entries[e.next].prev = e.prev;
    } else {
        memo.tail = e.prev;
    }
    e.prev = kSpecNone;
    e.next = kSpecNone;
}

void SpecMemoPushHead(SpecMemoState& memo, u32 i) {
    SpecMemoEntry& e = memo.entries[i];
    e.prev = kSpecNone;
    e.next = memo.head;
    if (memo.head != kSpecNone) {
        memo.entries[memo.head].prev = i;
    } else {
        memo.tail = i;
    }
    memo.head = i;
}
} // namespace

// sotc: SOTC_SPEC_MEMO. Stores the answer of a miss. The least recently used entry is replaced once
// the memo is full. Entries are never updated in place: a key that is stored again is a new entry.
static void SpecMemoStore(SpecMemoState& memo, u64 key_hash, const Program& program,
                          Shader::HwStage hw_stage, Shader::SwStage sw_stage, u64 hash,
                          const Shader::Backend::Bindings& start, const Shader::RuntimeInfo& rt,
                          bool has_tess, const Shader::TessellationDataConstantBuffer& tess,
                          const Shader::Gcn::FetchShaderData& fetch, const std::vector<u32>& words,
                          u32 perm) {
    if (memo.entries.empty()) {
        memo.entries.resize(SpecMemoState::kCap);
    }
    u32 i{};
    if (memo.used < SpecMemoState::kCap) {
        i = memo.used++;
        g_spec_memo.entries.store(memo.used, std::memory_order_relaxed);
    } else {
        i = memo.tail;
        SpecMemoUnlink(memo, i);
        const auto [first, last] = memo.index.equal_range(memo.entries[i].key_hash);
        for (auto it = first; it != last; ++it) {
            if (it->second == i) {
                memo.index.erase(it);
                break;
            }
        }
        g_spec_memo.evictions.fetch_add(1, std::memory_order_relaxed);
    }
    SpecMemoEntry& e = memo.entries[i];
    e.program = &program;
    e.hw_stage = hw_stage;
    e.sw_stage = sw_stage;
    e.hash = hash;
    e.perm = perm;
    e.key_hash = key_hash;
    e.start = start;
    e.runtime_info = rt;
    e.has_tess = has_tess;
    e.tess = tess;
    e.fetch = fetch;
    e.words = words;
    memo.index.emplace(key_hash, i);
    SpecMemoPushHead(memo, i);
}

// sotc: writes the SOTC_SPEC_MEMO line for the 5 s report: counts since the previous call. Prints
// nothing while the switch is unset or the window had no lookups. Call from one thread only.
void ReportSpecMemo(std::string& out, double seconds) {
    if (SpecMemoMode() == 0) {
        return;
    }
    // Same order as the fields of SpecMemoStats.
    const std::array<const std::atomic<u64>*, 10> counters{
        &g_spec_memo.lookups,         &g_spec_memo.hits,
        &g_spec_memo.stores,          &g_spec_memo.store_skipped,
        &g_spec_memo.evictions,       &g_spec_memo.probe_ns,
        &g_spec_memo.mismatch_perm,   &g_spec_memo.mismatch_module,
        &g_spec_memo.mismatch_fetch,  &g_spec_memo.mismatch_bindings,
    };
    static std::array<u64, counters.size()> prev{};
    std::array<u64, counters.size()> d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        const u64 now = counters[i]->load(std::memory_order_relaxed);
        d[i] = now - prev[i];
        prev[i] = now;
    }
    if (d[0] == 0) {
        return;
    }
    const u64 mismatches = d[6] + d[7] + d[8] + d[9];
    char line[512];
    std::snprintf(
        line, sizeof(line),
        "SOTC spec memo: mode %d, lookups %llu, hits %llu (%.1f%%), stores %llu (skipped %llu), "
        "evictions %llu, entries %llu/%zu, probe %.0f ns/s, mismatches %llu (perm %llu, module "
        "%llu, fetch %llu, bindings %llu)\n",
        SpecMemoMode(), static_cast<unsigned long long>(d[0]),
        static_cast<unsigned long long>(d[1]),
        100.0 * static_cast<double>(d[1]) / static_cast<double>(d[0]),
        static_cast<unsigned long long>(d[2]), static_cast<unsigned long long>(d[3]),
        static_cast<unsigned long long>(d[4]),
        static_cast<unsigned long long>(g_spec_memo.entries.load(std::memory_order_relaxed)),
        SpecMemoState::kCap,
        seconds > 0.0 ? static_cast<double>(d[5]) / seconds : 0.0,
        static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(d[6]),
        static_cast<unsigned long long>(d[7]), static_cast<unsigned long long>(d[8]),
        static_cast<unsigned long long>(d[9]));
    out += line;
}

// sotc: SOTC_PS_INPUT_NORMALIZE (on unless =0). The pixel inputs whose USE_DEFAULT flag can change the translation,
// as a mask of attribute indices, from the program's GCN code (decoded once per program hash): those read by
// v_interp_mov from another vertex than P0 (selector != 2). For a flat input (never IsDefault()) the flag is read
// only there, by IsPassthrough(), and only on devices with per-vertex attributes (V_INTERP_MOV_F32). The
// translator's own record of the reads (Info::loads) is not in the disk cache, so the code is the one source a fresh
// and a restored program share.
static uint64_t PassthroughSensitiveInputs(u64 hash, std::span<const u32> code) {
    static std::mutex mutex;
    static std::unordered_map<u64, uint64_t> masks;
    {
        std::scoped_lock lock{mutex};
        if (const auto it = masks.find(hash); it != masks.end()) {
            return it->second;
        }
    }
    uint64_t mask = 0;
    Shader::Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Shader::Gcn::GcnDecodeContext decoder;
    while (!slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(slice);
        if (inst.opcode == Shader::Gcn::Opcode::V_INTERP_MOV_F32 && inst.src[0].code != 2) {
            mask |= uint64_t{1} << inst.control.vintrp.attr;
        } else if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
            break;
        }
    }
    std::scoped_lock lock{mutex};
    masks.emplace(hash, mask);
    return mask;
}

// sotc: SOTC_PS_INPUT_NORMALIZE. The engine sets USE_DEFAULT on most flat pixel inputs and not on a few (13 slots
// of 4 particle programs, flat inputs read with v_interp_mov from P0), and it cannot be known before the draw. For a
// flat input it changes the SPIR-V only through PassthroughSensitiveInputs; elsewhere it was a key difference
// without a code difference. Such flat inputs are keyed as passthrough (USE_DEFAULT, value 0), the 99.6% case.
// Returns the slots changed.
static u32 NormalizeFlatInputs(Shader::RuntimeInfo& info, uint64_t sensitive_mask) {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PS_INPUT_NORMALIZE");
        return !v || v[0] != '0';
    }();
    if (!on) {
        return 0;
    }
    const u32 clip = info.hw.fs.clip_distance_emulation ? 1u : 0u;
    const u32 count = info.hw.fs.num_inputs > clip ? info.hw.fs.num_inputs - clip : 0;
    u32 changed = 0;
    for (u32 i = 0; i < count && i < info.hw.fs.inputs.size(); ++i) {
        auto& input = info.hw.fs.inputs[i];
        if (input.is_flat && !(sensitive_mask & (uint64_t{1} << i)) && (!input.is_default || input.default_value)) {
            input.is_default = true;
            input.default_value = 0;
            ++changed;
        }
    }
    return changed;
}

// sotc: SOTC_PRETRANSLATE. The key parts in which a draw's key differs from the pre-translated one (pretranslate.h).
static u32 PretranslateDiff(const Shader::StageSpecialization& a, const Shader::StageSpecialization& b) {
    using namespace SotcPretranslate;
    u32 diff = 0;
    const auto& ra = a.runtime_info;
    const auto& rb = b.runtime_info;
    if (!(ra.props == rb.props)) {
        diff |= DiffProps;
    }
    if (ra.sw_stage == SwStage::Fragment && rb.sw_stage == SwStage::Fragment) {
        const auto& fa = ra.hw.fs;
        const auto& fb = rb.hw.fs;
        if (!std::ranges::equal(fa.color_buffers, fb.color_buffers) || fa.mrtz_mask != fb.mrtz_mask ||
            fa.num_samples != fb.num_samples) {
            diff |= DiffLayout;
        }
        const u32 n = std::min<u32>(fa.num_inputs, 32);
        if (!(fa.en_flags == fb.en_flags) || !(fa.addr_flags == fb.addr_flags) || fa.num_inputs != fb.num_inputs ||
            !std::equal(fa.inputs.begin(), fa.inputs.begin() + n, fb.inputs.begin())) {
            diff |= DiffPsInputs;
        }
        const u32 other = (fa.z_export_format != fb.z_export_format ? DiffPsZExport : 0u) |
                          (fa.front_face_all_bits != fb.front_face_all_bits ? DiffPsFrontFace : 0u) |
                          (fa.dual_source_blending != fb.dual_source_blending ? DiffPsDualSource : 0u) |
                          (fa.clip_distance_emulation != fb.clip_distance_emulation ? DiffPsClip : 0u) |
                          (fa.early_fragment_tests != fb.early_fragment_tests ? DiffPsEarlyZ : 0u);
        if (other) {
            diff |= DiffPsOther | other;
        }
    } else if (ra.sw_stage == SwStage::Vertex && rb.sw_stage == SwStage::Vertex) {
        const auto& ha = ra.hw.vs;
        const auto& hb = rb.hw.vs;
        if (!(ha == hb)) {
            diff |= DiffVsHw;
            diff |= (ha.num_outputs != hb.num_outputs || ha.outputs != hb.outputs ? DiffVsOutputs : 0u) |
                    (ha.clip_disable != hb.clip_disable ? DiffVsClipDisable : 0u) |
                    (ha.emulate_depth_negative_one_to_one != hb.emulate_depth_negative_one_to_one
                         ? DiffVsDepthEmulation
                         : 0u) |
                    (ha.user_clip_plane_mask != hb.user_clip_plane_mask ? DiffVsUserClip : 0u);
        }
        const auto& sa = ra.sw.vs;
        const auto& sb = rb.sw.vs;
        if (!(sa == sb)) {
            diff |= DiffVsSw;
            diff |= (sa.step_rate_0 != sb.step_rate_0 || sa.step_rate_1 != sb.step_rate_1 ? DiffVsStepRate : 0u) |
                    (sa.vertex_sgpr_offset != sb.vertex_sgpr_offset ? DiffVsVertexSgpr : 0u) |
                    (sa.instance_sgpr_offset != sb.instance_sgpr_offset ? DiffVsInstanceSgpr : 0u) |
                    (sa.tess_emulated_primitive != sb.tess_emulated_primitive ? DiffVsTessEmulated : 0u);
        }
    } else if (!(ra == rb)) {
        diff |= DiffProps;
    }
    if (!(a.fetch_shader_data == b.fetch_shader_data) || a.vs_attribs != b.vs_attribs) {
        diff |= DiffFetch;
    }
    if (a.buffers != b.buffers) {
        diff |= DiffBuffers;
    }
    if (a.images != b.images) {
        diff |= DiffImages;
        for (size_t i = 0; i < a.images.size() && i < b.images.size(); ++i) {
            const auto& ia = a.images[i];
            const auto& ib = b.images[i];
            diff |= (ia.type != ib.type ? DiffImgType : 0u) | (ia.is_integer != ib.is_integer ? DiffImgInteger : 0u) |
                    (ia.is_srgb != ib.is_srgb ? DiffImgSrgb : 0u) |
                    (!(ia.dst_select == ib.dst_select) ? DiffImgSwizzle : 0u) |
                    (ia.num_conversion != ib.num_conversion ? DiffImgConversion : 0u) |
                    (ia.num_bindings != ib.num_bindings ? DiffImgBindings : 0u) |
                    (ia.is_storage != ib.is_storage ? DiffImgStorage : 0u);
        }
    }
    if (a.samplers != b.samplers) {
        diff |= DiffSamplers;
    }
    if (a.fmasks != b.fmasks) {
        diff |= DiffFmasks;
    }
    if (!(a.buffers.empty() && a.images.empty() && a.samplers.empty()) && a.start != b.start) {
        diff |= DiffStart;
    }
    return diff;
}

// sotc: SOTC_PRETRANSLATE. The register state a program's file declares: SPI_SHADER_PGM_RSRC1/2 (the layout of
// AmdGpu::ShaderProgram::settings, RSRC1 in the low word) into the props BuildRuntimeInfo fills from them.
static void PretranslateProps(Shader::RuntimeInfo& rt, u32 rsrc1, u32 rsrc2) {
    AmdGpu::ShaderProgram program{};
    const u64 settings = (u64{rsrc2} << 32) | rsrc1;
    static_assert(sizeof(program.settings) == sizeof(settings));
    std::memcpy(&program.settings, &settings, sizeof(settings));
    rt.props.num_user_data = program.settings.num_user_regs;
    rt.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
    rt.props.num_allocated_vgprs = program.NumVgprs();
    rt.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
    rt.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
    rt.props.fp_round_mode32 = program.settings.fp_round_mode32;
    rt.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
}

void PipelineCache::PrecompileCheck() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last_print{};
    const auto now = Clock::now();
    const u64 queued = SotcPretranslate::JobsQueued();
    const u64 done = SotcPretranslate::JobsDone();
    if (now - last_print > std::chrono::seconds{2}) {
        last_print = now;
        std::fprintf(stderr, "SOTC PRECOMPILE: %llu of %llu shader translations done\n",
                     static_cast<unsigned long long>(done), static_cast<unsigned long long>(queued));
        std::fflush(stderr);
    }
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    const s64 last_request = SotcPretranslate::LastRequestNs();
    const SotcCache::PreparationStatus status{
        .queued = queued,
        .completed = done,
        .translating = pre_in_flight,
        .compute_builds = pre_builds_pending.load(std::memory_order_acquire),
        .backlog = pre_backlog.size(),
        .requests_waiting = SotcPretranslate::Pending(),
        .deferred = !pre_deferred.empty(),
        .last_request_ns = last_request,
        .now_ns = now_ns,
    };
    if (!status.ReadyToFinalize()) {
        return;
    }
    // sotc: precompile bypasses MaybeHoldForPretranslate: prepare resident stage libraries here too,
    // before PrePackFinish / _Exit destroys their only opportunity to populate the driver cache.
    if (!BuildPackLibraries()) {
        std::fprintf(stderr, "SOTC PRECOMPILE failed: GPL library preparation incomplete\n");
        std::fflush(stderr);
        Storage::DataBase::Instance().Close();
        std::_Exit(1); // sotc: the launcher must not stamp a failed preparation
    }
    // Every program the game loaded is translated and in the cache: the pack, the index, the cache files, and out.
    PrePackFinish();
    SaveShaderIndex();
    // sotc: the run ends with _Exit, which skips the destructor: the driver cache (the pipelines built by this run,
    // compute ones included) is written here, as the 60 s timer would have done.
    SaveDriverCache();
    std::fprintf(stderr, "SOTC PRECOMPILE done: %llu shader translations written to the shader cache\n",
                 static_cast<unsigned long long>(done));
    std::fflush(stderr);
    Storage::DataBase::Instance().Close();
    std::fflush(stdout);
    std::_Exit(0);
}

void PipelineCache::DrainPretranslate() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::vector<SotcPretranslate::Request> requests;
    if (SotcPretranslate::Pending()) {
        requests = SotcPretranslate::Take();
    }
    // Waiting vertex requests are looked at again every 100 ms (their pixel program installs meanwhile).
    if (!pre_deferred.empty() && (now - pre_deferred_since > std::chrono::milliseconds{100} || !requests.empty())) {
        for (auto& r : pre_deferred) {
            requests.push_back(std::move(r));
        }
        pre_deferred.clear();
    }
    if (requests.empty()) {
        return;
    }
    // A program variant (SOTC_SHADER_ADDR) mixes its variant into the hash: those graphics keys are not made here. The
    // requests are counted as dropped (before this check they vanished silently: queued 0 in every such run).
    // sotc: SOTC_SHADER_ADDR (table mode): compute requests are prebuilt with the table key (QueueComputePrebuild
    // makes the StageVariantPlain hash RefreshComputeKey looks up). A spec-free compute program is determined by its
    // code, so all of them are built at the loading hold, as in descriptor mode (the drop left 0 prebuilt programs and
    // every first dispatch compiled cold).
    if (profile.shader_addr != 0) {
        for (auto& r : requests) {
            if (r.stage == 2) {
                QueueComputePrebuild(r);
            } else {
                SotcPretranslate::NoteDropped(r.stage == 1 ? 1 : 0, 2);
            }
        }
        FeedPretranslate();
        return;
    }
    // sotc: SOTC_PRETRANSLATE_COMPUTE: compute requests take their own path (QueueComputePrebuild).
    for (auto& r : requests) {
        if (r.stage == 2) {
            QueueComputePrebuild(r);
        }
    }
    std::erase_if(requests, [](const auto& r) { return r.stage == 2; });
    if (requests.empty()) {
        FeedPretranslate();
        return;
    }
    static const std::array<u32, Shader::ShaderParams::NumShaderUserData> zeros{};
    // Pixel requests first: a vertex request pairs with a pixel program, often of the same batch.
    std::stable_partition(requests.begin(), requests.end(), [](const auto& r) { return r.stage == 0; });
    for (auto& r : requests) {
        const int stage = r.stage == 1 ? 1 : 0;
        if (IsProgramKnown(r.hash)) {
            if (r.deferred_ns == 0) {
                SotcPretranslate::NoteDropped(stage, 0);
            }
            if (SotcPretranslate::Precompile()) {
                // sotc: a forced preparation of warm SPIR-V must recreate its modules too, otherwise
                // the GPL pack sees only drawn programs and a missing driver cache stays unprepared.
                LoadLazyProgram(r.hash); // loading-only process: no gameplay deadline to block
            } else if (SotcNonBlocking::Prefetch() && !(pre_holding && pre_stream_evicted_gfx.contains(r.hash))) {
                QueueLazyPrefetch(r.hash); // sotc: SOTC_PRETRANSLATE_STREAM: not one the hold just sent to the index
            }
            continue;
        }
        const HwStage hw = stage == 1 ? HwStage::Vertex : HwStage::Fragment;
        const SwStage sw = stage == 1 ? SwStage::Vertex : SwStage::Fragment;
        // The draw state the files do not give (clip settings, the vertex step rates, Z export and early Z of the
        // pixel stage...) is the last draw's: the game sets most of it the same way for all its draws.
        Shader::RuntimeInfo rt = runtime_infos[u32(sw)];
        if (rt.hw_stage != hw || rt.sw_stage != sw) {
            rt.Initialize(hw, sw);
        }
        PretranslateProps(rt, r.rsrc1, r.rsrc2);
        if (stage == 1) {
            AmdGpu::VsOutputControl ctl{};
            std::memcpy(&ctl, &r.regs[21 - 16], sizeof(ctl));
            rt.hw.vs.num_outputs = MapOutputs(rt.hw.vs.outputs, ctl);
            // A triangle draw with clipping on (the last draw's state was often a rect list: emulated, clip off).
            rt.hw.vs.clip_disable = false;
            rt.hw.vs.user_clip_plane_mask = 0;
            rt.sw.vs.tess_emulated_primitive = false;
            // The game's instanced draws are indirect and put the start instance in user SGPR 2 (0x4c + 2,
            // UdRegFromShOffset); its other draws are direct (no SGPR). A vertex program that loads the instance ID
            // VGPR (RSRC1.VGPR_COMP_CNT 3) is an instanced one.
            rt.sw.vs.vertex_sgpr_offset = 0;
            rt.sw.vs.instance_sgpr_offset = rt.props.num_input_vgprs >= 3 ? 0x4c + 2 : 0;
            // sotc: SOTC_VS_DRAW_KIND: the shared module serves both kinds (the second variant below is not made).
            rt.sw.vs.instance_sgpr_add = VsDrawKindShared(hw, rt.props.num_input_vgprs, 0, rt.sw.vs.instance_sgpr_offset);
        }
        Shader::Backend::Bindings start{};
        if (stage == 0) {
            auto& fs = rt.hw.fs;
            std::memcpy(&fs.en_flags, &r.ena, sizeof(r.ena));
            std::memcpy(&fs.addr_flags, &r.addr, sizeof(r.addr));
            fs.num_inputs = std::min<u32>(r.num_inputs, u32(fs.inputs.size()));
            fs.inputs = {};
            for (u32 i = 0; i < fs.num_inputs; ++i) {
                // As the key holds them after SOTC_PS_INPUT_NORMALIZE: an input read with v_interp_mov only is a
                // passthrough (flat, default 0).
                const bool interpolated = (r.interp_mask >> i) & 1;
                fs.inputs[i] = {.param_index = r.param[i],
                                .is_default = !interpolated,
                                .is_flat = !interpolated,
                                .default_value = 0};
            }
            fs.dual_source_blending = false;
            fs.clip_distance_emulation = false;
            SotcPredict::PixelLayout(r.hash, rt, 0);
            AmdGpu::BarycentricControl baryc{};
            AmdGpu::DepthShaderControl db{};
            std::memcpy(&baryc, &r.regs[24 - 16], sizeof(baryc));
            std::memcpy(&db, &r.regs[25 - 16], sizeof(db));
            fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(r.regs[19 - 16] & 0xF);
            fs.front_face_all_bits = baryc.front_face_all_bits;
            const u8 stencil_export = db.stencil_op_val_export_enable | db.stencil_test_val_export_enable;
            fs.mrtz_mask = db.z_export_enable | (stencil_export << 1) | (db.mask_export_enable << 2) |
                           (db.coverage_to_mask_enable << 3);
            fs.early_fragment_tests = Common::ElfInfo::Instance().IsSotc() &&
                                      (db.z_order == AmdGpu::ZOrder::EarlyZLateZ ||
                                       db.z_order == AmdGpu::ZOrder::EarlyZReZ) &&
                                      (fs.mrtz_mask & 0b111) == 0;
        } else if (Shader::Backend::VsBindingBaseOn()) {
            start = Shader::Backend::VsStartBindings({}, profile.stage_bindings); // SOTC_VS_BINDING_BASE
        } else if (r.pair != 0) {
            // The vertex stage's bindings start where its pixel stage's end (the pixel stage is bound first).
            const auto pair = program_cache.find(r.pair);
            const auto left = pair == program_cache.end() ? pre_stream_pair_start.find(r.pair)
                                                          : pre_stream_pair_start.end();
            if (left != pre_stream_pair_start.end()) {
                // sotc: SOTC_PRETRANSLATE_STREAM: the pixel program is on disk; its bindings were kept when it left.
                start = left->second;
                if (profile.stage_bindings) {
                    start.unified = 0;
                }
            } else if (pair == program_cache.end()) {
                const bool waiting = pending_programs.contains(r.pair);
                if (waiting && (r.deferred_ns == 0 || now_ns - r.deferred_ns < 10'000'000'000LL)) {
                    if (r.deferred_ns == 0) {
                        r.deferred_ns = now_ns;
                    }
                    if (pre_deferred.empty()) {
                        pre_deferred_since = now;
                    }
                    pre_deferred.push_back(std::move(r));
                    continue;
                }
                SotcPretranslate::NoteDropped(stage, 1);
                continue;
            }
            if (left == pre_stream_pair_start.end()) {
                const auto& pair_info = pair.value()->info;
                pair_info.AddBindings(start, Shader::UsesBindlessSamplers(pair_info));
                if (profile.stage_bindings) {
                    start.unified = 0;
                }
            }
        }
        // The variants of the key the file leaves open: every color layout of the pixel program's pass; both draw
        // kinds of an instanced vertex program (direct, or indirect with the start instance in user SGPR 2).
        boost::container::small_vector<Shader::RuntimeInfo, 8> variants{rt};
        if (stage == 0) {
            const size_t layouts = std::min<size_t>(SotcPredict::PixelLayoutCount(r.hash), 8);
            for (size_t l = 1; l < layouts; ++l) {
                Shader::RuntimeInfo v = rt;
                SotcPredict::PixelLayout(r.hash, v, l);
                v.hw.fs.mrtz_mask = rt.hw.fs.mrtz_mask; // the program's own DB_SHADER_CONTROL
                if (std::ranges::find(variants, v) == variants.end()) {
                    variants.push_back(v);
                }
            }
        } else if (rt.sw.vs.instance_sgpr_offset != 0 && !rt.sw.vs.instance_sgpr_add) {
            Shader::RuntimeInfo v = rt;
            v.sw.vs.instance_sgpr_offset = 0;
            variants.push_back(v);
        }
        for (size_t vi = 0; vi < variants.size(); ++vi) {
            auto job = std::make_unique<AsyncPipelineJob>();
            auto& t = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
            t.hw_stage = hw;
            t.sw_stage = sw;
            t.hash = r.hash;
            t.user_data = zeros;
            t.owned_code = r.code; // the first variant's job may queue one more with the same code
            t.code = t.owned_code;
            t.runtime_info = variants[vi];
            t.start = start;
            t.pretranslate = true;
            t.first_variant = true; // each layout variant gets its integer-verdict twin
            SotcPretranslate::NoteQueued(stage);
            PreJobQueued(r.hash); // sotc: SOTC_PRETRANSLATE_STREAM
            pre_backlog.push_back(std::move(job));
        }
        pending_programs.insert(r.hash);
        pre_pending.insert(r.hash);
    }
    FeedPretranslate();
}

void PipelineCache::FeedPretranslate() {
    const u32 limit = pre_holding || SotcPretranslate::Precompile() ? 64u : 2u;
    if (pre_backlog.empty() || pre_in_flight >= limit) {
        return;
    }
    // sotc: SOTC_GPL_PREDICT: during play a pack translation waits while library jobs are queued or running (they are
    // for programs a material named; PredictLibraries feeds the backlog again once they are done).
    if (SotcNonBlocking::GplPredict() && !pre_holding && !SotcPretranslate::Precompile() &&
        lib_jobs_pending.load(std::memory_order_relaxed) != 0) {
        g_nodrop.pre_deferred.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        std::scoped_lock lock{async_mutex};
        while (!pre_backlog.empty() && pre_in_flight < limit) {
            PushJobLocked(JobLane::Background, std::move(pre_backlog.front())); // behind every other job
            pre_backlog.pop_front();
            ++pre_in_flight;
        }
    }
    async_cv.notify_all();
}

// sotc: SOTC_PRETRANSLATE_COMPUTE. A compute program's first dispatch computes its key with the stage's plain
// params (RefreshComputeKey); the prebuilt pipeline uses the same key: HashCombine(hash, 0), the first permutation.
void PipelineCache::QueueComputePrebuild(SotcPretranslate::Request& r) {
    // sotc: SOTC_SHADER_ADDR (table mode): the dispatch looks the program up by its plain variant (RefreshComputeKey:
    // variant StageVariantPlain, guest_hash the game's hash, hash HashCombine(hash, variant)); the prebuild uses that
    // program hash for the program cache, the pack, the index and the pipeline key. Descriptor mode: the game's hash.
    const bool table = profile.shader_addr != 0;
    // sotc: SOTC_HEAP: a heap profile binds the heap variant first (RefreshComputeKey: variant StageVariantHeap, hash
    // HashCombine(hash, variant)) unless the candidate was refused before (heap_refused); the prebuild makes that key.
    // A candidate the translated Info refuses (Heap::StageRefused) is prebuilt again as the plain variant at install
    // (InstallComputePrebuild), as the dispatch falls back. Heap off: as before (descriptor and plain table mode).
    const bool heap_candidate = profile.heap_images && profile.support_int64 &&
                                !heap_refused.contains(HashCombine(r.hash, Shader::StageVariantHeap));
    const u8 variant = heap_candidate ? Shader::StageVariantHeap
                       : table        ? Shader::StageVariantPlain
                                      : Shader::StageVariantDefault;
    const u64 program_hash = variant != Shader::StageVariantDefault ? HashCombine(r.hash, variant) : r.hash;
    const ComputePipelineKey key{HashCombine(program_hash, size_t{0})};
    if (IsProgramKnown(program_hash) || lazy_programs.contains(program_hash) || compute_pipelines.contains(key)) {
        SotcPretranslate::NoteDropped(2, 0); // known: a warm session has it (as the other requests)
        // sotc: loaded from the pack by a worker before its first dispatch. SOTC_COMPILE_STREAM: a released program's
        // file loaded again: the pack load is queued in any case, and its install rebuilds the pipelines on workers.
        // Not during the hold that released it (it is rebuilt at the hold's end, StreamRebuildBeforePlay).
        const bool released = stream_released_programs.contains(program_hash);
        if ((SotcNonBlocking::Prefetch() || released) && !(pre_holding && released)) {
            if (!QueueLazyPrefetch(program_hash)) {
                StreamRehydrate(program_hash); // resident already (no pack load needed)
            }
        }
        return;
    }
    // sotc: SOTC_COMPUTE_PACK_MAX: a shader set beyond the cap is not prebuilt (the rest build at their dispatch).
    if (SotcNonBlocking::ComputePackRequested() && pack_compute_queued >= SotcNonBlocking::ComputePackMax()) {
        ++pack_compute_capped;
        SotcPretranslate::NoteDropped(2, 3);
        return;
    }
    ++pack_compute_queued;
    const SotcPretranslate::ComputeFields f = SotcPretranslate::ComputeFieldsOf(r.regs, r.cs_layout);
    // The registers as the dispatch's ComputeProgram holds them: settings is RSRC1 then RSRC2 (regs_shader.h).
    AmdGpu::ComputeProgram cs{};
    const u32 settings[2] = {f.rsrc1, f.rsrc2};
    if (sizeof(cs.settings) != sizeof settings) {
        SotcPretranslate::NoteDropped(2, 3);
        return;
    }
    std::memcpy(&cs.settings, settings, sizeof settings);
    cs.num_thread_x.full = static_cast<u16>(f.nx);
    cs.num_thread_y.full = static_cast<u16>(f.ny);
    cs.num_thread_z.full = static_cast<u16>(f.nz);
    auto rt = runtime_infos[u32(SwStage::Compute)]; // not default-constructible
    rt.Initialize(HwStage::Compute, SwStage::Compute);
    FillComputeRuntimeInfo(rt, cs);

    pre_compute_pred[r.hash] = PreComputePrediction{f.rsrc1, f.rsrc2, f.nx, f.ny, f.nz, r.regs, r.cs_layout};
    auto job = std::make_unique<AsyncPipelineJob>();
    auto& t = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
    t.hw_stage = HwStage::Compute;
    t.sw_stage = SwStage::Compute;
    t.hash = program_hash;
    t.variant = variant; // sotc: SOTC_SHADER_ADDR / SOTC_HEAP (0 in descriptor mode)
    t.guest_hash = variant != Shader::StageVariantDefault ? r.hash : 0; // sotc: Info::pgm_hash stays the game's hash
    t.owned_code = r.code;
    t.code = t.owned_code;
    t.runtime_info = rt;
    t.pretranslate = true;
    SotcPretranslate::NoteQueued(2);
    PreJobQueued(program_hash); // sotc: SOTC_COMPILE_STREAM
    pre_backlog.push_back(std::move(job));
}

void PipelineCache::InstallComputePrebuild(AsyncPipelineJob::Translate& t) {
    if (pre_in_flight > 0) {
        --pre_in_flight;
    }
    struct FeedNext {
        PipelineCache& cache;
        ~FeedNext() {
            cache.FeedPretranslate();
        }
    } feed_next{*this};
    // sotc: SOTC_COMPUTE_SPECFREE: the permutation the prebuild serves (0, or an int/float variant's index).
    size_t perm_idx = 0;
    const auto fail = [&] {
        SotcPretranslate::NoteBuilt(2, false);
        if (t.synthetic_variant == 0) {
            pre_compute_pred.erase(t.guest_hash != 0 ? t.guest_hash : t.hash); // sotc: keyed by the game's hash
        }
        if (t.module) {
            instance.GetDevice().destroyShaderModule(t.module);
            t.module = vk::ShaderModule{};
        }
    };
    if (t.failed || !t.new_program || !t.spec ||
        (t.synthetic_variant == 0 && compute_pipelines.contains(ComputePipelineKey{HashCombine(t.hash, size_t{0})}))) {
        fail();
        return;
    }
    // sotc: SOTC_HEAP: the heap candidate's decision is made from its translated Info, as RefreshComputeKey makes it. A
    // refused candidate is noted (its dispatch then binds the plain variant without translating it) and its program is
    // prebuilt again as the variant that dispatch binds: plain in table mode, the default in descriptor mode.
    if (t.variant == Shader::StageVariantHeap && t.synthetic_variant == 0 &&
        SotcArch::Heap::StageRefused(t.new_program->info)) {
        NoteHeapRefused(t.new_program->info.CacheHash());
        if (t.module) {
            instance.GetDevice().destroyShaderModule(t.module);
            t.module = vk::ShaderModule{};
        }
        const u64 guest = t.guest_hash;
        const u8 plain_variant = profile.shader_addr != 0 ? Shader::StageVariantPlain : Shader::StageVariantDefault;
        const u64 plain_hash = plain_variant != Shader::StageVariantDefault ? HashCombine(guest, plain_variant) : guest;
        if (IsProgramKnown(plain_hash) || lazy_programs.contains(plain_hash) ||
            compute_pipelines.contains(ComputePipelineKey{HashCombine(plain_hash, size_t{0})})) {
            SotcPretranslate::NoteDropped(2, 0); // known: a warm session has the plain variant
            const bool released = stream_released_programs.contains(plain_hash);
            if ((SotcNonBlocking::Prefetch() || released) && !(pre_holding && released)) {
                if (!QueueLazyPrefetch(plain_hash)) {
                    StreamRehydrate(plain_hash); // sotc: SOTC_COMPILE_STREAM, resident already
                }
            }
            return;
        }
        auto job = std::make_unique<AsyncPipelineJob>();
        auto& v = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
        v.hw_stage = t.hw_stage;
        v.sw_stage = t.sw_stage;
        v.hash = plain_hash;
        v.variant = plain_variant;
        v.guest_hash = plain_variant != Shader::StageVariantDefault ? guest : 0;
        v.owned_code = t.owned_code;
        v.code = v.owned_code;
        v.runtime_info = t.runtime_info;
        v.start = t.start;
        v.pretranslate = true;
        g_specfree.heap_refused_prebuilds.fetch_add(1, std::memory_order_relaxed);
        PreJobQueued(plain_hash); // sotc: SOTC_COMPILE_STREAM
        pre_backlog.push_back(std::move(job)); // counted queued once, as the candidate (FeedNext sends it)
        return;
    }
    // sotc: SOTC_COMPUTE_SPECFREE: a program whose images the code alone does not type (sotc_synthetic_reads) gets the
    // other int/float verdicts as more prebuilds (variant 1), compiled at load like the first: every combination of its
    // k ambiguous images (masks 1..2^k-1) when k <= kSpecFreeMaxAmbiguous, else the all-flipped one only.
    const bool queue_variant = Shader::ComputeSpecFree() && t.synthetic_variant == 0 &&
                               t.new_program->info.sotc_synthetic_reads;
    const u32 ambiguous = t.new_program->info.sotc_synthetic_ambiguous;
    Program* program = nullptr;
    if (const auto found = program_cache.find(t.hash); found != program_cache.end()) {
        // A draw translated the program meanwhile: its module serves the pipeline when the specialization is the same.
        program = found.value().get();
        const auto same = std::ranges::find(program->modules, *t.spec, &Program::Module::spec);
        if (same != program->modules.end() && (t.synthetic_variant == 0 ? same == program->modules.begin() : true)) {
            perm_idx = static_cast<size_t>(std::distance(program->modules.begin(), same));
            instance.GetDevice().destroyShaderModule(t.module);
            t.module = vk::ShaderModule{};
        } else if (t.synthetic_variant == 1 && !program->modules.empty() &&
                   SameBindingLayout(program->info, t.new_program->info) &&
                   std::ranges::find(program->pending_specs, *t.spec) == program->pending_specs.end()) {
            // sotc: SOTC_COMPUTE_SPECFREE: the variant is one more permutation of the program.
            perm_idx = program->modules.size();
            Shader::CombineBindlessCache(program->info.bindless, t.new_program->info.bindless);
            if (!((pre_holding || SotcPretranslate::Precompile()) &&
                  PrePackAdd(t.hash, static_cast<u32>(perm_idx),
                             SerializeShaderMeta(program->info, *t.spec, HashCombine(t.hash, perm_idx), perm_idx),
                             t.spv))) {
                RegisterShaderBinary(std::move(t.spv), t.hash, perm_idx);
                SotcPredict::SetPretranslating(true);
                RegisterShaderMeta(program->info, t.spec->fetch_shader_data, *t.spec, HashCombine(t.hash, perm_idx),
                                   perm_idx);
                SotcPredict::SetPretranslating(false);
            }
            program->AddPermut(t.module, std::move(*t.spec));
            g_specfree.variants.fetch_add(1, std::memory_order_relaxed);
        } else {
            fail();
            return;
        }
        // A module the compute pack destroyed (a program evicted to the index) cannot make a pipeline.
        if (compute_pipelines.contains(ComputePipelineKey{HashCombine(t.hash, perm_idx)}) ||
            !program->modules[perm_idx].module) {
            SotcPretranslate::NoteBuilt(2, false);
            return;
        }
    } else if (t.synthetic_variant != 0) {
        fail(); // sotc: SOTC_COMPUTE_SPECFREE: the first prebuild of the program did not install
        return;
    } else {
        auto [it, is_new] = program_cache.try_emplace(t.hash);
        it.value() = std::move(t.new_program);
        program = it.value().get();
        program->sotc_pre = true;
        // Stored as the graphics pre-translations are (the hold's pack, or loose files and the meta).
        if (!((pre_holding || SotcPretranslate::Precompile()) &&
              PrePackAdd(t.hash, 0, SerializeShaderMeta(program->info, *t.spec, HashCombine(t.hash, 0), 0), t.spv))) {
            RegisterShaderBinary(std::move(t.spv), t.hash, 0);
            SotcPredict::SetPretranslating(true);
            RegisterShaderMeta(program->info, t.spec->fetch_shader_data, *t.spec, HashCombine(t.hash, 0), 0);
            SotcPredict::SetPretranslating(false);
        }
        program->AddPermut(t.module, std::move(*t.spec));
        program->info.UserData() = {};
        if (Shader::ComputeSpecFree()) {
            g_specfree.programs.fetch_add(1, std::memory_order_relaxed);
            g_specfree.variants.fetch_add(1, std::memory_order_relaxed);
        }
        if (t.variant == Shader::StageVariantHeap) {
            g_specfree.heap_prebuilds.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_HEAP
        }
    }
    // sotc: SOTC_COMPUTE_SPECFREE: the variant masks of the program (0: the all-flipped variant 1 of an over-cap one).
    boost::container::small_vector<u8, 8> variant_masks;
    if (queue_variant) {
        if (ambiguous != 0 && ambiguous <= Shader::kSpecFreeMaxAmbiguous) {
            for (u32 mask = 1; mask < (1u << ambiguous); ++mask) {
                variant_masks.push_back(static_cast<u8>(mask));
            }
        } else {
            variant_masks.push_back(0);
        }
    }
    if (Shader::ComputeSpecFree() && t.synthetic_variant == 0) {
        const bool over_cap = queue_variant && ambiguous > Shader::kSpecFreeMaxAmbiguous;
        const size_t perms = 1 + variant_masks.size();
        const size_t bucket = over_cap ? 4 : perms >= 8 ? 3 : perms >= 4 ? 2 : perms >= 2 ? 1 : 0;
        g_specfree.perms_hist[bucket].fetch_add(1, std::memory_order_relaxed);
        g_specfree.perms_queued.fetch_add(perms, std::memory_order_relaxed);
        program->sotc_ambiguous = static_cast<u8>(std::min<u32>(ambiguous, 255u));
        static std::atomic<u32> over_cap_logged{0};
        if (over_cap && over_cap_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
            std::fprintf(stderr,
                         "SOTC COMPUTE SPECFREE over cap %#llx: %u ambiguous images (2 variants prebuilt, not %llu)\n",
                         static_cast<unsigned long long>(t.hash), ambiguous, 1ull << std::min<u32>(ambiguous, 63u));
        }
    }
    for (const u8 mask : variant_masks) {
        auto job = std::make_unique<AsyncPipelineJob>();
        auto& v = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
        v.hw_stage = t.hw_stage;
        v.sw_stage = t.sw_stage;
        v.hash = t.hash;
        v.variant = t.variant;       // sotc: SOTC_SHADER_ADDR: the table variant of the first prebuild
        v.guest_hash = t.guest_hash;
        v.owned_code = t.owned_code;
        v.code = v.owned_code;
        v.runtime_info = t.runtime_info;
        v.start = t.start;
        v.pretranslate = true;
        v.synthetic_variant = 1;
        v.synthetic_mask = mask;
        ++pack_compute_queued;
        SotcPretranslate::NoteQueued(2);
        PreJobQueued(t.hash); // sotc: SOTC_COMPILE_STREAM
        pre_backlog.push_back(std::move(job));
    }
    const ComputePipelineKey key{HashCombine(t.hash, perm_idx)};
    // The ComputePipeline is made on the command processor (its layouts and the heap state are CP-only); a worker
    // builds it (Build). Ordinary pipeline: SerializationSupport, RegisterPipelineData, the same as a draw's.
    // sotc: SOTC_COMPUTE_PACK: a prebuild of the hold (or of the preparation run) is compiled into the pack cache and
    // destroyed by the compute pack; its pipeline key is not registered (the next session must not warm it up: its
    // dispatch builds it from the pack cache, inline).
    // sotc: SOTC_COMPILE_STREAM: the hold's prebuilds take the same path (the pack step releases them), but their key
    // is registered as an ordinary prebuild's: the next session's warm-up builds them at its loading screen, since
    // without SOTC_COMPUTE_PACK no dispatch builds them from the pack cache.
    const bool stream_candidate = SotcNonBlocking::CompileStream() && !SotcNonBlocking::ComputePack() &&
                                  (pre_holding || !pre_hold_done || SotcPretranslate::Precompile());
    const bool pack_candidate = stream_candidate || (SotcNonBlocking::ComputePack() &&
                                                     (pre_holding || !pre_hold_done || SotcPretranslate::Precompile()));
    ComputePipeline::SerializationSupport sdata{};
    auto pipe = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, layout_cache,
                                                  StageProfile(program->info.variant),
                                                  pack_candidate ? PackPipelineCache() : *pipeline_cache, key,
                                                  program->info, program->modules[perm_idx].module, sdata, false,
                                                  true);
    ComputePipeline* raw = pipe.get();
    auto [cit, cnew] = compute_pipelines.try_emplace(key);
    cit.value() = std::move(pipe);
    if (pack_candidate) {
        raw->sotc_pack = true;
        raw->sotc_pack_program = t.hash;
        if (t.hinted) {
            pack_compute_hinted.insert(t.hash);
        }
        s64 expected = 0;
        pack_compute_first_ns.compare_exchange_strong(
            expected,
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count(),
            std::memory_order_relaxed);
    }
    if (!pack_candidate || stream_candidate) {
        RegisterPipelineData(key, sdata);
    }
    if (SotcNonBlocking::ComputeRelease()) {
        // sotc: SOTC_COMPUTE_RELEASE: its module leaves once the pipeline is built (ReleaseBuiltComputeModules).
        compute_release_queue.push_back({t.hash, static_cast<u32>(perm_idx), key.value});
    }
    ++num_new_pipelines;
    if (async_workers.empty()) {
        raw->Build();
        SotcPretranslate::NoteBuilt(2, raw->Handle() != vk::Pipeline{});
        return;
    }
    auto job = std::make_unique<AsyncPipelineJob>();
    job->compute = raw;
    job->pre_compute = true;
    job->pack_compute = pack_candidate;
    pre_builds_pending.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock{async_mutex};
        // sotc: SOTC_NB_SCHED: outside the boot hold a prebuild is background work (a dispatch that binds it before a
        // worker took it builds it on the recorder, ComputePipeline::BindWhenReady).
        PushJobLocked(SotcNonBlocking::Sched() && !pre_holding ? JobLane::Background : JobLane::Compute,
                      std::move(job));
    }
    async_cv.notify_one();
}

// sotc: SOTC_PRETRANSLATE_COMPUTE. The dispatch's ComputeProgram against the prediction (once per program): the
// settings dwords are RSRC1 and RSRC2, the thread counts the full part of NUM_THREAD_X/Y/Z.
void PipelineCache::NoteComputeDispatch(const AmdGpu::ComputeProgram& cs, u64 hash) {
    const auto it = pre_compute_pred.find(hash);
    if (it == pre_compute_pred.end()) {
        return;
    }
    const PreComputePrediction p = it->second;
    pre_compute_pred.erase(it);
    u32 real[2]{};
    std::memcpy(real, &cs.settings, sizeof real);
    const u32 nx = cs.num_thread_x.full, ny = cs.num_thread_y.full, nz = cs.num_thread_z.full;
    const bool same = real[0] == p.rsrc1 && real[1] == p.rsrc2 && nx == p.nx && ny == p.ny && nz == p.nz;
    SotcPretranslate::NoteVerify(same);
    if (same) {
        return;
    }
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::fprintf(stderr,
                     "SOTC PRETRANSLATE compute verify mismatch %#llx (layout %u): predicted rsrc1 %08x rsrc2 %08x "
                     "threads %u %u %u, dispatch rsrc1 %08x rsrc2 %08x threads %u %u %u; descriptor %08x %08x %08x "
                     "%08x %08x %08x %08x %08x\n",
                     static_cast<unsigned long long>(hash), p.layout, p.rsrc1, p.rsrc2, p.nx, p.ny, p.nz, real[0],
                     real[1], nx, ny, nz, p.raw[0], p.raw[1], p.raw[2], p.raw[3], p.raw[4], p.raw[5], p.raw[6],
                     p.raw[7]);
    }
}

namespace {
// sotc: SOTC_GPL_PACK_LIBS. sotc_pack_libraries.bin in the versioned cache folder (so a shader cache version change
// voids it): a header with the driver identity, then the ids (PackLibraryId) of the stage libraries whose compiled code
// the saved driver cache holds. Bump kPackLibVersion when the library create infos change.
constexpr u32 kPackLibMagic = 0x42494C50; // "PLIB"
constexpr u32 kPackLibVersion = 3; // sotc: 2 bound the list to the persisted cache bytes; 3 adds the kind bit (compute)
struct PackLibHeader {
    u32 magic{kPackLibMagic};
    u32 version{kPackLibVersion};
    u32 vendor{};
    u32 device{};
    u32 driver{};
    u32 dyn_bits{};
    std::array<u8, VK_UUID_SIZE> uuid{};
    u64 count{};
    u64 cache_hash{};
};
std::filesystem::path PackLibListPath() {
    return Storage::DataBase::Instance().CachePath() / "sotc_pack_libraries.bin";
}
// sotc: writes the list (header h with count and cache_hash set, then the ids) through a temporary file.
bool WritePackLibList(PackLibHeader h, const std::vector<u64>& ids) {
    h.count = ids.size();
    const auto path = PackLibListPath();
    auto tmp = path;
    tmp += ".tmp";
    std::FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 &&
                    (ids.empty() || std::fwrite(ids.data(), sizeof(u64), ids.size(), f) == ids.size());
    const bool closed = std::fclose(f) == 0;
    std::error_code ec;
    if (ok && closed) {
        std::filesystem::rename(tmp, path, ec);
        return !ec;
    }
    std::filesystem::remove(tmp, ec);
    return false;
}
PackLibHeader PackLibIdentity(const Instance& instance) {
    PackLibHeader h;
    h.vendor = instance.GetVendorID();
    h.device = instance.GetDeviceID();
    h.driver = instance.GetDriverVersion();
    const auto& kd = instance.KeyDynamic();
    h.dyn_bits = u32{kd.topology} | (u32{kd.polygon_mode} << 1) | (u32{kd.depth_clamp} << 2) |
                 (u32{kd.depth_clip} << 3) | (u32{kd.provoking} << 4) | (u32{kd.clip_negative} << 5) |
                 (u32{kd.samples} << 6) | (u32{instance.IsDescriptorBufferEnabled()} << 7);
    const auto& uuid = instance.GetPipelineCacheUUID();
    std::copy(uuid.begin(), uuid.end(), h.uuid.begin());
    return h;
}
// sotc: reads the ids of the list file into `out` when its header is `identity` with the cache hash `cache_hash`
// (the caller checked PackLibraryListMatches). False when the file is missing or another identity.
bool ReadPackLibList(const PackLibHeader& identity, u64 cache_hash, std::unordered_set<u64>& out) {
    std::FILE* f = _wfopen(PackLibListPath().c_str(), L"rb");
    if (!f) {
        return false;
    }
    PackLibHeader h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == identity.magic && h.version == identity.version &&
              h.vendor == identity.vendor && h.device == identity.device && h.driver == identity.driver &&
              h.dyn_bits == identity.dyn_bits && h.uuid == identity.uuid && h.cache_hash == cache_hash;
    if (ok) {
        std::vector<u64> ids(static_cast<size_t>(std::min<u64>(h.count, u64{1} << 22)));
        if (!ids.empty() && std::fread(ids.data(), sizeof(u64), ids.size(), f) == ids.size()) {
            out.insert(ids.begin(), ids.end());
        }
    }
    std::fclose(f);
    return ok;
}
} // namespace

// sotc: a valid list must describe this exact driver-cache snapshot and a complete list file.
// Header compatibility alone does not prove that the driver retained any particular library.
bool PipelineCache::PackLibraryListMatches() const {
    if (!driver_cache_loaded) {
        return false;
    }
    std::FILE* f = _wfopen(PackLibListPath().c_str(), L"rb");
    if (!f) {
        return false;
    }
    PackLibHeader h{};
    const bool read = std::fread(&h, sizeof h, 1, f) == 1;
    std::fclose(f);
    const PackLibHeader identity = PackLibIdentity(instance);
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(PackLibListPath(), ec);
    return read && !ec && h.magic == identity.magic && h.version == identity.version &&
           h.vendor == identity.vendor && h.device == identity.device && h.driver == identity.driver &&
           h.dyn_bits == identity.dyn_bits && h.uuid == identity.uuid && h.cache_hash == loaded_driver_cache_hash &&
           h.count <= (u64{1} << 22) && bytes == sizeof h + h.count * sizeof(u64);
}

bool PipelineCache::BuildPackLibraries() {
    using Clock = std::chrono::steady_clock;
    using L = GplLibraryCache;
    // sotc: distinguish deliberate/unsupported skips from a successful pack build in every run.
    const char* skip = !SotcNonBlocking::GplPackLibs() ? "GPL pack disabled or shader-only keys unavailable"
                       : !DriverCacheEnabled() ? "SOTC_DRIVER_CACHE=0 (pack persistence requires the driver cache)"
                       : async_workers.empty() ? "no pipeline workers"
                       : instance.IsDescriptorHeapEnabled() ? "descriptor heap mode"
                       : !instance.IsVertexInputDynamicState() ? "dynamic vertex input unavailable"
                       : EmulatorSettings.IsShaderCollect() ? "shader collection mode" : nullptr;
    if (skip) {
        std::fprintf(stderr, "SOTC GPL PACK LIBS: skipped: %s\n", skip);
        return true; // deliberate or unsupported: no claim that libraries were prepared
    }
    const auto start = Clock::now();
    const uint64_t private_start = SotcPipeManifest::ProcessPrivateBytes();
    LogRam("pack step start"); // sotc: SOTC RAM
    // sotc: SOTC_GPL_PACK_LEAN: the pack runs after the manifest pipelines and the compute prebuilds, which are already
    // on the workers (collected here; the manifest's translations wait for its own pump and are not waited for).
    const bool lean = SotcNonBlocking::GplPackLean();
    if (lean) {
        const auto busy = [&] {
            if (pre_builds_pending.load(std::memory_order_acquire) != 0) {
                return true;
            }
            if (!manifest) {
                return false;
            }
            const auto& m = *manifest;
            return m.graphics_building != 0 || m.libs_building != 0 ||
                   g_manifest_stats.compute_built.load(std::memory_order_relaxed) < m.compute_queued;
        };
        while (busy() && Clock::now() - start < std::chrono::seconds{120}) {
            if (async_done_count.load(std::memory_order_acquire) != 0) {
                CollectAsyncPipelines();
            }
            std::unique_lock lock{async_mutex};
            translate_done_cv.wait_for(lock, std::chrono::milliseconds{20},
                                       [this] { return async_done_count.load(std::memory_order_relaxed) != 0; });
        }
    }
    const auto wait_end = Clock::now();
    // The list is trusted only with the driver cache it describes (loaded and accepted at startup) and the same driver.
    const PackLibHeader identity = PackLibIdentity(instance);
    std::unordered_set<u64> listed;
    bool list_void = !PackLibraryListMatches();
    if (!list_void && !ReadPackLibList(identity, loaded_driver_cache_hash, listed)) {
        list_void = true;
    }
    // A partner per stage variant for the other stage of each job: a vertex and a pixel program that a per-shader link
    // can take (no bindless samplers). Its own libraries are only looked up.
    struct Partner {
        Program* program{};
        size_t perm{};
    };
    std::unordered_map<u32, std::array<Partner, 2>> partners; // variant -> {vertex, pixel}
    std::unordered_set<const Program*> partner_programs; // sotc: SOTC_GPL_PACK_LEAN: their modules stay (every job)
    // sotc: SOTC_PACK_REFERENCED: the programs a material the game has created so far names (its VS and PS footer
    // hashes; queued materials are read without taking them) and the manifest's stages. A program matches by its
    // program key, its program hash or its cache hash (a variant's program is named by its hash only).
    const bool restrict_refs = SotcNonBlocking::GplPackReferenced();
    std::unordered_set<u64> referenced;
    if (restrict_refs) {
        SotcPipePredict::ReferencedPrograms(referenced);
        if (manifest) {
            for (const auto& st : manifest->stages) {
                referenced.insert(st.rec.hash);
                referenced.insert(st.cache_hash);
            }
        }
    }
    const auto is_referenced = [&](u64 key, const Program& p) {
        return !restrict_refs || referenced.contains(key) || referenced.contains(p.info.pgm_hash) ||
               referenced.contains(p.info.CacheHash());
    };
    u64 programs_all = 0, programs_ref = 0, unreferenced = 0;
    const auto usable = [](const Program& p, size_t& perm) {
        // Hardware VS and PS only: an export/local vertex stage is linked with geometry or tessellation stages, which a
        // pack job does not have (its library would never match a draw's).
        const bool vs = p.info.sw_stage == Shader::SwStage::Vertex && p.info.hw_stage == Shader::HwStage::Vertex;
        const bool ps = p.info.sw_stage == Shader::SwStage::Fragment && p.info.hw_stage == Shader::HwStage::Fragment;
        if ((!vs && !ps) || Shader::UsesBindlessSamplers(p.info)) {
            return false;
        }
        for (size_t i = 0; i < p.modules.size(); ++i) {
            if (p.modules[i].module) {
                perm = i;
                return true;
            }
        }
        return false;
    };
    for (auto it = program_cache.begin(); it != program_cache.end(); ++it) {
        Program& p = *it.value();
        size_t perm = 0;
        if (!usable(p, perm)) {
            continue;
        }
        ++programs_all;
        programs_ref += is_referenced(it.key(), p) ? 1 : 0;
        auto& slot = partners[u32{p.info.variant}][p.info.sw_stage == Shader::SwStage::Vertex ? 0 : 1];
        if (!slot.program) {
            slot = Partner{&p, perm};
            partner_programs.insert(&p);
        }
    }
    if (lean) {
        StartExtraWorkers(); // sotc: SOTC_GPL_PACK_LEAN: every hardware thread while the screen is held
        pack_touched.clear();
        std::scoped_lock lock{pack_mutex};
        pack_done_modules.clear();
    }
    const GraphicsPipelineKey rest = DefaultLibraryRest();
    u64 queued = 0, skipped = 0, no_partner = 0, unlinkable = 0, queued_vs = 0;
    {
        std::scoped_lock lock{pack_mutex};
        pack_compiled.clear();
    }
    pack_failed.store(0, std::memory_order_relaxed);
    // sotc: pack_compiled + pack_failed counts this batch only; other library jobs may complete too.
    // sotc: the library jobs of one program (its key in the program cache); the count it queued.
    const auto queue_program = [&](u64 key, Program& p) -> u64 {
        size_t first = 0;
        if (!usable(p, first)) {
            return 0;
        }
        if (!is_referenced(key, p)) {
            ++unreferenced; // sotc: SOTC_PACK_REFERENCED: no material the game created names this program
            return 0;
        }
        const u64 queued_before = queued;
        const bool vertex = p.info.sw_stage == Shader::SwStage::Vertex;
        const auto found = partners.find(u32{p.info.variant});
        const Partner* partner = found == partners.end() ? nullptr : &found->second[vertex ? 1 : 0];
        for (size_t perm = 0; perm < p.modules.size(); ++perm) {
            if (!p.modules[perm].module) {
                continue;
            }
            const u32 part = vertex ? L::PreRaster : L::Fragment;
            const u64 id = PackLibraryId(p.info.CacheHash(), perm, part);
            if (listed.contains(id)) {
                ++skipped;
                continue;
            }
            if (!partner || !partner->program) {
                ++no_partner;
                continue;
            }
            auto job = vertex ? MakeStageLibraryJob(rest, p, perm, *partner->program, partner->perm, 1u << part,
                                                    L::OriginPack)
                              : MakeStageLibraryJob(rest, *partner->program, partner->perm, p, perm, 1u << part,
                                                    L::OriginPack);
            if (!job) {
                ++unlinkable;
                continue;
            }
            job->pack_id = id;
            job->pack_program = key; // sotc: SOTC_GPL_PACK_LEAN
            job->pack_perm = static_cast<u32>(perm);
            ++queued;
            queued_vs += vertex;
            lib_jobs_pending.fetch_add(1, std::memory_order_relaxed);
            {
                std::scoped_lock lock{async_mutex};
                PushJobLocked(JobLane::Predict, std::move(job));
            }
        }
        return queued - queued_before;
    };
    for (auto it = program_cache.begin(); it != program_cache.end(); ++it) {
        queue_program(it.key(), *it.value());
    }
    async_cv.notify_all();
    urgent_cv.notify_all(); // sotc: SOTC_GPL_URGENT_WORKERS: the Predict lane is theirs too
    const auto queued_end = Clock::now();
    // The loading screen stays up until every job ran (they hold the modules the eviction destroys).
    SotcStats::prep_total.store(static_cast<u32>(queued), std::memory_order_relaxed);
    auto last_log = start;
    const auto completed = [&]() -> u64 {
        std::scoped_lock lock{pack_mutex};
        return pack_compiled.size() + pack_failed.load(std::memory_order_relaxed);
    };
    // sotc: SOTC_GPL_PACK_LEAN: a pack job's module leaves memory as soon as its library is compiled. Not a partner's
    // (every job looks the partner up), not a drawn program's (sotc_pre_checked), not one with a translation in flight.
    // The eviction at the end of the hold sends the touched programs back to the index (lazy_programs) as it does for
    // every prepared program; it skips the destroyed handles (null).
    u64 modules_destroyed = 0;
    const auto drain_modules = [&] {
        std::vector<std::pair<u64, u32>> done;
        {
            std::scoped_lock lock{pack_mutex};
            done.swap(pack_done_modules);
        }
        for (const auto& [hash, perm] : done) {
            const auto found = program_cache.find(hash);
            if (found == program_cache.end()) {
                continue;
            }
            Program& p = *found.value();
            if (!p.sotc_pre || p.sotc_pre_checked || !p.pending_specs.empty() || partner_programs.contains(&p) ||
                perm >= p.modules.size() || !p.modules[perm].module) {
                continue;
            }
            instance.GetDevice().destroyShaderModule(p.modules[perm].module);
            p.modules[perm].module = vk::ShaderModule{};
            pack_touched.insert(hash);
            ++modules_destroyed;
        }
    };
    // sotc: SOTC_PRETRANSLATE_STREAM: the programs the hold sent to the index are loaded back by the workers (8 at a
    // time, while fewer than 256 library jobs wait), their libraries queued as each one arrives, and each one goes back
    // to the index once its modules were destroyed (every library compiled). The partners stay (pre_stream_keepers).
    std::vector<u64> stream_list;
    std::unordered_map<u64, u32> stream_counts;
    std::unordered_set<u64> stream_programs;
    size_t stream_next = 0, stream_loading = 0;
    if (lean && !pre_stream_evicted_gfx.empty()) {
        for (const u64 h : pre_stream_evicted_gfx) {
            const auto l = lazy_programs.find(h);
            if (l != lazy_programs.end() && !program_cache.contains(h)) {
                stream_list.push_back(h);
                stream_counts[h] = l->second;
            }
        }
        std::ranges::sort(stream_list);
        pack_streaming = !stream_list.empty();
        lazy_in_flight_max = 8;
    }
    const auto stream_feed = [&] {
        while (stream_next < stream_list.size() && stream_loading < 16 &&
               queued - std::min(queued, completed()) < 256) {
            const u64 h = stream_list[stream_next++];
            if (program_cache.contains(h) || !lazy_programs.contains(h)) {
                continue;
            }
            pack_stream_wanted.insert(h);
            ++stream_loading;
            if (!QueueLazyPrefetch(h)) {
                pack_stream_wanted.erase(h);
                LoadLazyProgram(h); // its loose files (not in the pack index): here, the screen is held
                pack_stream_loaded.push_back(h);
            }
        }
    };
    const auto stream_take = [&] {
        std::vector<u64> loaded;
        loaded.swap(pack_stream_loaded);
        for (const u64 h : loaded) {
            stream_loading -= std::min<size_t>(stream_loading, 1);
            const auto found = program_cache.find(h);
            if (found == program_cache.end()) {
                lazy_programs.try_emplace(h, stream_counts[h]); // a read failure: it stays on disk
                continue;
            }
            Program& p = *found.value();
            p.sotc_pre = true; // loaded during the hold: still a prepared program (the eviction takes it)
            ++pre_stream_reloaded;
            size_t perm = 0;
            if (usable(p, perm)) {
                ++programs_all;
                programs_ref += is_referenced(h, p) ? 1 : 0;
            }
            if (queue_program(h, p) == 0 && !partner_programs.contains(&p)) {
                // Nothing to compile for it (listed, unreferenced): its modules leave now.
                for (auto& m : p.modules) {
                    if (m.module) {
                        instance.GetDevice().destroyShaderModule(m.module);
                        m.module = vk::ShaderModule{};
                    }
                }
            }
            stream_programs.insert(h);
        }
        if (!loaded.empty()) {
            async_cv.notify_all();
            urgent_cv.notify_all();
        }
    };
    const auto stream_evict = [&] {
        if (stream_programs.empty() ||
            lib_jobs_pending.load(std::memory_order_acquire) > queued - std::min(queued, completed())) {
            return; // another library job runs (it may hold any program)
        }
        std::optional<LiveProgramRefs> refs;
        bool evicted = false;
        for (auto it = stream_programs.begin(); it != stream_programs.end();) {
            const auto found = program_cache.find(*it);
            if (found == program_cache.end()) {
                it = stream_programs.erase(it);
                continue;
            }
            Program& p = *found.value();
            if (partner_programs.contains(&p) ||
                std::ranges::any_of(p.modules, [](const Program::Module& m) { return bool(m.module); })) {
                ++it; // libraries still compiling (or one failed: the end-of-hold eviction decides)
                continue;
            }
            if (!refs) {
                refs = CollectLiveProgramRefs();
            }
            if (p.sotc_pre_checked || !p.pending_specs.empty() || pending_programs.contains(*it) ||
                refs->Referenced(*it, p)) {
                it = stream_programs.erase(it); // stays resident; the end-of-hold eviction decides
                continue;
            }
            lazy_programs[*it] = std::max(static_cast<u32>(p.modules.size()), stream_counts[*it]);
            program_cache.erase(found);
            ++pre_stream_reevicted;
            evicted = true;
            it = stream_programs.erase(it);
        }
        if (evicted) {
            last_graphics_pipeline = nullptr;
            pipe_fast_gfx.valid = false;
        }
    };
    int pack_decile = 0;
    while ((completed() < queued || stream_next < stream_list.size() || stream_loading != 0) &&
           Clock::now() - start < std::chrono::minutes{30}) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            CollectAsyncPipelines();
        }
        if (pack_streaming) {
            stream_take();
            stream_feed();
        }
        if (lean) {
            drain_modules();
            if (pack_streaming) {
                stream_evict();
            }
        }
        const u64 left = queued - std::min(queued, completed());
        SotcStats::prep_total.store(static_cast<u32>(queued), std::memory_order_relaxed);
        SotcStats::prep_done.store(static_cast<u32>(queued - std::min<u64>(queued, left)), std::memory_order_relaxed);
        // sotc: SOTC RAM: one line per tenth of the pack step (jobs done; streamed programs loaded).
        if (queued != 0) {
            const u64 job_tenths = (queued - left) * 10 / queued;
            const u64 load_tenths = stream_list.empty() ? 10 : stream_next * 10 / stream_list.size();
            const int decile = static_cast<int>(std::min<u64>(job_tenths, load_tenths));
            if (decile > pack_decile) {
                pack_decile = decile;
                char where[64];
                std::snprintf(where, sizeof where, "pack step %d%%", decile * 10);
                LogRam(where);
            }
        }
        if (Clock::now() - last_log > std::chrono::seconds{5}) {
            last_log = Clock::now();
            std::fprintf(stderr, "SOTC GPL PACK LIBS: compiling %.1f s, %llu of %llu left%s\n",
                         std::chrono::duration<double>(last_log - start).count(), static_cast<unsigned long long>(left),
                         static_cast<unsigned long long>(queued),
                         stream_list.empty() ? ""
                                             : fmt::format("; programs loaded back {} of {}", stream_next,
                                                           stream_list.size()).c_str());
        }
        std::unique_lock lock{async_mutex};
        translate_done_cv.wait_for(lock, std::chrono::milliseconds{20},
                                   [this] { return async_done_count.load(std::memory_order_relaxed) != 0; });
    }
    if (async_done_count.load(std::memory_order_acquire) != 0) {
        CollectAsyncPipelines();
    }
    const size_t extra_count = extra_workers.size();
    if (lean) {
        drain_modules();
        if (pack_streaming) {
            stream_take();
            stream_evict();
        }
        StopExtraWorkers(); // back to the normal worker count for play
    }
    if (pack_streaming) {
        std::fprintf(stderr,
                     "SOTC PRETRANSLATE STREAM: pack step loaded back %llu programs (of %zu on disk), %llu went back to "
                     "the index after their libraries, %zu left to the end-of-hold eviction\n",
                     static_cast<unsigned long long>(pre_stream_reloaded), stream_list.size(),
                     static_cast<unsigned long long>(pre_stream_reevicted), stream_programs.size());
    }
    pack_streaming = false;
    pack_stream_wanted.clear();
    pack_stream_loaded.clear();
    lazy_in_flight_max = 2;
    if (completed() < queued || stream_next < stream_list.size()) {
        std::fprintf(stderr, "SOTC GPL PACK LIBS: incomplete: %llu of %llu jobs finished (programs loaded back %zu of "
                             "%zu); no list published\n",
                     static_cast<unsigned long long>(completed()), static_cast<unsigned long long>(queued), stream_next,
                     stream_list.size());
        return false;
    }
    const auto compiled_end = Clock::now();
    std::vector<u64> compiled;
    {
        std::scoped_lock lock{pack_mutex};
        compiled.swap(pack_compiled);
    }
    // sotc: SOTC_COMPUTE_PACK: the hold's compute prebuilds (already in the pack cache) join the list; their pipelines
    // and modules leave memory here, before the publication names them.
    // sotc: SOTC_COMPILE_STREAM: the same step releases the stream prebuilds (PackComputePipelines records them).
    if (lean && (SotcNonBlocking::ComputePack() || SotcNonBlocking::CompileStream())) {
        PackComputePipelines(listed, compiled, start);
    }
    // The driver cache is written now (the list may only name code the file holds), then the list.
    bool list_written = false;
    bool publish_background = false; // sotc: SOTC_GPL_PACK_LEAN: the saver thread finishes it after the screen drops
    size_t cache_kb = 0; // sotc: only read the persisted size under the saver mutex
    const auto publish_start = Clock::now();
    if (!compiled.empty() || (list_void && !listed.empty())) {
        if (list_void) {
            listed.clear();
        }
        listed.insert(compiled.begin(), compiled.end());
        std::vector<u64> ids(listed.begin(), listed.end());
        if (lean && driver_cache_saver.joinable()) {
            // sotc: SOTC_GPL_PACK_LEAN: on the saver thread, the cache then the list. The hold waits for it while it is
            // short (3 s); a longer write finishes in the background (the order is kept: same thread). The precompile
            // run waits for it whole (it exits right after).
            {
                std::scoped_lock lk{driver_cache_cv_mutex};
                pack_publish_ids = std::move(ids);
                pack_publish_header.resize(sizeof identity);
                std::memcpy(pack_publish_header.data(), &identity, sizeof identity);
                pack_publish_state.store(1, std::memory_order_release);
                driver_cache_save_requested = true;
            }
            driver_cache_cv.notify_all();
            {
                std::unique_lock lk{driver_cache_cv_mutex};
                const auto done = [this] { return pack_publish_state.load(std::memory_order_acquire) >= 2; };
                if (SotcPretranslate::Precompile()) {
                    pack_publish_cv.wait(lk, done);
                } else {
                    pack_publish_cv.wait_for(lk, std::chrono::seconds{3}, done);
                }
            }
            const u32 state = pack_publish_state.load(std::memory_order_acquire);
            list_written = state == 2;
            publish_background = state == 1;
            if (list_written) {
                std::scoped_lock lk{driver_cache_save_mutex};
                cache_kb = saved_driver_cache_size >> 10;
            }
        } else {
            // The size with every pack library in it; a save from here on holds them (other pipelines built meanwhile
            // only make it larger).
            size_t written_size = 0;
            u64 written_hash = 0;
            const bool saved = SaveDriverCache(true, &written_size, &written_hash);
            cache_kb = written_size >> 10; // sotc: success is the actual write, never a size comparison
            if (saved) {
                PackLibHeader h = identity;
                h.cache_hash = written_hash;
                list_written = WritePackLibList(h, ids);
            }
        }
    }
    if (!list_void) {
        // sotc: SOTC_PLAY_QUIET: the exit publication rewrites this list (with the pack cache's new hash); a pending
        // publication replaces it with the full list.
        std::scoped_lock lk{driver_cache_cv_mutex};
        if (pack_listed_ids.empty()) {
            pack_listed_ids.assign(listed.begin(), listed.end());
        }
    }
    // sotc: SOTC_COMPUTE_PACK: the dispatches of this session see the compiled set (in the pack cache object now,
    // whatever the state of the file); an earlier lazy load of the file is replaced.
    pack_listed_set = listed;
    pack_listed_set_loaded = true;
    if (lean) {
        std::fprintf(stderr,
                     "SOTC GPL PACK LEAN: waited for manifest/compute builds %.1f s; queued %llu jobs in %.2f s; "
                     "compiled %.1f s on %zu workers (%zu extra for the hold); modules destroyed %llu (programs %zu); "
                     "publish %s (%.1f s)\n",
                     std::chrono::duration<double>(wait_end - start).count(), static_cast<unsigned long long>(queued),
                     std::chrono::duration<double>(queued_end - wait_end).count(),
                     std::chrono::duration<double>(compiled_end - queued_end).count(),
                     async_workers.size() + extra_count, extra_count, static_cast<unsigned long long>(modules_destroyed),
                     pack_touched.size(),
                     list_written ? "written on the saver thread"
                     : publish_background ? "finishing in the background (list after the cache)"
                                          : "not needed or failed",
                     std::chrono::duration<double>(Clock::now() - publish_start).count());
    }
    const uint64_t private_end = SotcPipeManifest::ProcessPrivateBytes();
    LogRam("pack step end"); // sotc: SOTC RAM
    std::fprintf(stderr,
                 "SOTC GPL PACK LIBS: %llu stage libraries compiled into the driver cache (vertex %llu, pixel %llu) in "
                 "%.1f s, failed %llu; already compiled (listed) %llu%s; skipped: no partner %llu, not linkable %llu; "
                 "kept in memory 0 (destroyed after compile, made again from the cache when their area loads); driver "
                 "cache %zu MB, list %s; process private %.0f MB (%+.0f MB); total %.1f s\n",
                 static_cast<unsigned long long>(compiled.size()),
                 static_cast<unsigned long long>(queued_vs), static_cast<unsigned long long>(queued - queued_vs),
                 std::chrono::duration<double>(compiled_end - start).count(),
                 static_cast<unsigned long long>(pack_failed.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(skipped),
                 list_void ? " (no valid list: driver cache missing, rejected or another driver)" : "",
                 static_cast<unsigned long long>(no_partner), static_cast<unsigned long long>(unlinkable),
                 cache_kb >> 10, list_written ? "written" : publish_background ? "writing in the background" : "unchanged",
                 static_cast<double>(private_end) / 1048576.0,
                 (static_cast<double>(private_end) - static_cast<double>(private_start)) / 1048576.0,
                 std::chrono::duration<double>(Clock::now() - start).count());
    if (restrict_refs) {
        pack_ref.pack_done = true;
        pack_ref.materials = SotcPipePredict::Stats().queued;
        pack_ref.programs = programs_ref;
        pack_ref.programs_all = programs_all;
        pack_ref.libs_at_load = compiled.size();
        pack_ref.load_seconds = std::chrono::duration<double>(compiled_end - start).count();
        std::fprintf(stderr,
                     "SOTC PACK REFERENCED: %llu programs of %llu loaded are named by a material or the manifest; "
                     "the other %llu are not compiled at load (a material naming one queues its libraries when it is "
                     "created)\n",
                     static_cast<unsigned long long>(programs_ref), static_cast<unsigned long long>(programs_all),
                     static_cast<unsigned long long>(unreferenced));
        PackReferencedLog();
    }
    return pack_failed.load(std::memory_order_relaxed) == 0 && no_partner == 0 && unlinkable == 0 &&
           (compiled.empty() || list_written || publish_background);
}

// sotc: SOTC_COMPUTE_PACK. The compute prebuilds of the hold are already compiled (the lean pack waited for
// pre_builds_pending == 0 before its library jobs; a straggler installed since is waited for here, bounded). Each one
// is listed when the driver accepted it; its pipeline and module are destroyed unless a dispatch bound the program
// (sotc_pre_checked) or a permutation is in flight; the program is then evicted to the index with the others.
void PipelineCache::PackComputePipelines(const std::unordered_set<u64>& listed, std::vector<u64>& compiled,
                                         std::chrono::steady_clock::time_point start) {
    using Clock = std::chrono::steady_clock;
    const auto wait_start = Clock::now();
    while (pre_builds_pending.load(std::memory_order_acquire) != 0 &&
           Clock::now() - wait_start < std::chrono::seconds{60}) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            CollectAsyncPipelines();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    u64 programs = 0, built = 0, hinted = 0, synthetic = 0, failed = 0, already = 0, resident = 0, destroyed = 0;
    // sotc: SOTC_COMPILE_STREAM: the prebuilds PretranslateStreamStep released during the hold (in the pack cache).
    for (const u64 id : stream_compiled_ids) {
        if (!listed.contains(id)) {
            compiled.push_back(id);
        }
    }
    stream_compiled_ids.clear();
    std::vector<ComputePipelineKey> gone;
    // sotc: SOTC_COMPUTE_SPECFREE: a program can have several pack pipelines (the int/float variants). Its modules stay
    // while either one is still on a worker (the build reads the module).
    std::unordered_set<u64> busy_programs;
    for (auto it = compute_pipelines.begin(); it != compute_pipelines.end(); ++it) {
        if (it.value()->sotc_pack && !it.value()->IsReady()) {
            busy_programs.insert(it.value()->sotc_pack_program);
        }
    }
    for (auto it = compute_pipelines.begin(); it != compute_pipelines.end(); ++it) {
        ComputePipeline& pipe = *it.value();
        if (!pipe.sotc_pack) {
            continue; // a dispatch's, the manifest's or the warm-up's: resident as before, and not in the pack cache
        }
        ++programs;
        const u64 id = PackComputeId(it.key().value);
        if (!pipe.IsReady()) {
            ++failed; // still on a worker after the bounded wait: not listed (its build may still land in the cache)
            continue;
        }
        const bool ok = pipe.Handle() != vk::Pipeline{};
        if (!ok) {
            ++failed;
        } else if (listed.contains(id)) {
            ++already;
        } else {
            ++built;
            compiled.push_back(id);
            if (pack_compute_hinted.contains(pipe.sotc_pack_program)) {
                ++hinted;
            } else {
                ++synthetic;
            }
        }
        const auto found = program_cache.find(pipe.sotc_pack_program);
        Program* program = found == program_cache.end() ? nullptr : found.value().get();
        if (!program || !program->sotc_pre || program->sotc_pre_checked || !program->pending_specs.empty() ||
            program->modules.empty() || busy_programs.contains(pipe.sotc_pack_program)) {
            ++resident; // bound by a dispatch (or a translation in flight): the pipeline stays, as before
            continue;
        }
        // The pipeline (made, never bound) and the module leave memory; the program follows at the eviction.
        gone.push_back(it.key());
        if (ok && SotcNonBlocking::CompileStream()) {
            // sotc: SOTC_COMPILE_STREAM: rebuilt from the pack cache on the workers when its program loads again
            // (StreamRehydrate); its shader file's next load requests it again.
            stream_released_keys.insert(it.key().value);
            if (stream_released_programs.insert(pipe.sotc_pack_program).second) {
                SotcPretranslate::Forget(program->info.pgm_hash);
            }
            ++stream_released;
        }
        if (ok) {
            for (auto& module : program->modules) {
                if (module.module) {
                    instance.GetDevice().destroyShaderModule(module.module);
                    module.module = vk::ShaderModule{};
                }
            }
            pack_touched.insert(pipe.sotc_pack_program);
        }
        ++destroyed;
    }
    for (const auto& key : gone) {
        compute_pipelines.erase(key);
    }
    if (!gone.empty()) {
        last_compute_pipeline = nullptr; // the memo and the fast record see a smaller map
        pipe_fast_cs.valid = false;
    }
    pack_compute_hinted.clear();
    const s64 first = pack_compute_first_ns.load(std::memory_order_relaxed);
    const s64 last = pack_compute_last_ns.load(std::memory_order_relaxed);
    const double seconds = first != 0 && last > first ? static_cast<double>(last - first) / 1e9 : 0.0;
    char capped[96] = "";
    if (pack_compute_capped != 0) {
        std::snprintf(capped, sizeof capped, "; capped: %llu programs beyond SOTC_COMPUTE_PACK_MAX=%u not prebuilt",
                      static_cast<unsigned long long>(pack_compute_capped), SotcNonBlocking::ComputePackMax());
    }
    std::fprintf(stderr,
                 "SOTC COMPUTE PACK: programs %llu, built %llu (hints %llu, synthetic %llu) in %.1f s (first install "
                 "to last build; worker time %.1f s), failed %llu, already listed %llu, kept resident %llu (destroyed "
                 "%llu; pack step %.2f s, %.1f s into the pack)%s\n",
                 static_cast<unsigned long long>(programs), static_cast<unsigned long long>(built),
                 static_cast<unsigned long long>(hinted), static_cast<unsigned long long>(synthetic), seconds,
                 static_cast<double>(pack_compute_build_ns.load(std::memory_order_relaxed)) / 1e9,
                 static_cast<unsigned long long>(failed), static_cast<unsigned long long>(already),
                 static_cast<unsigned long long>(resident), static_cast<unsigned long long>(destroyed),
                 std::chrono::duration<double>(Clock::now() - wait_start).count(),
                 std::chrono::duration<double>(Clock::now() - start).count(), capped);
    if (SotcNonBlocking::CompileStream()) {
        StreamLog(true);
    }
}

bool PipelineCache::ProgramHasComputePipeline(u64 program_key, const Program& program) const {
    // Both keys a compute pipeline of the program can carry: the program cache key (prebuilds, dispatches) and its
    // cache hash (the manifest's); they are equal unless a variant mixed its hash in differently.
    const u64 cache_hash = program.info.CacheHash();
    for (size_t perm = 0; perm < std::max<size_t>(program.modules.size(), 1); ++perm) {
        if (compute_pipelines.contains(ComputePipelineKey{HashCombine(program_key, perm)}) ||
            (cache_hash != program_key && compute_pipelines.contains(ComputePipelineKey{HashCombine(cache_hash, perm)}))) {
            return true;
        }
    }
    return false;
}

void PipelineCache::ReleaseBuiltComputeModules(bool force) {
    if (!SotcNonBlocking::ComputeRelease() || compute_release_queue.empty() || EmulatorSettings.IsShaderCollect()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - compute_release_last < std::chrono::seconds{1}) {
        return;
    }
    compute_release_last = now;
    u64 released = 0;
    size_t keep = 0;
    for (size_t i = 0; i < compute_release_queue.size(); ++i) {
        const ComputeRelease e = compute_release_queue[i];
        const auto prog = program_cache.find(e.program);
        const auto pipe = compute_pipelines.find(ComputePipelineKey{e.key});
        if (prog == program_cache.end() || pipe == compute_pipelines.end() ||
            e.perm >= prog.value()->modules.size()) {
            continue; // the program or the pipeline left (pack step, eviction): nothing to release
        }
        Program& program = *prog.value();
        auto& m = program.modules[e.perm];
        if (!m.module) {
            continue; // released already, or destroyed by the compute pack
        }
        ComputePipeline& pipeline = *pipe.value();
        if (!program.pending_specs.empty() || !pipeline.IsReady() || pipeline.OnFastBuild()) {
            compute_release_queue[keep++] = e; // still building (or a translation of the program in flight)
            continue;
        }
        if (!pipeline.ModuleReleasable()) {
            continue; // refused by the driver: the module stays (a later build of the key may need it)
        }
        instance.GetDevice().destroyShaderModule(m.module);
        m.module = vk::ShaderModule{};
        m.sotc_released = true;
        pipeline.ReleaseModule();
        ++released;
    }
    compute_release_queue.resize(keep);
    if (released != 0) {
        pipe_fast_cs.valid = false; // the record holds the module handle of its stage
        compute_modules_released += released;
    }
    if (force) {
        std::fprintf(stderr,
                     "SOTC COMPUTE RELEASE: %llu modules destroyed now (%llu this session; pipelines built), %zu "
                     "waiting for their build\n",
                     static_cast<unsigned long long>(released),
                     static_cast<unsigned long long>(compute_modules_released), compute_release_queue.size());
    }
}

void PipelineCache::PreJobQueued(u64 hash) {
    ++pre_jobs_inflight[hash];
}

void PipelineCache::PreJobDone(u64 hash) {
    const auto it = pre_jobs_inflight.find(hash);
    if (it != pre_jobs_inflight.end() && --it->second == 0) {
        pre_jobs_inflight.erase(it);
    }
}

bool PipelineCache::LiveProgramRefs::Referenced(u64 program_key, const Program& program) const {
    if (programs.contains(&program) || infos.contains(&program.info)) {
        return true;
    }
    const u64 cache_hash = program.info.CacheHash();
    for (size_t perm = 0; perm < std::max<size_t>(program.modules.size(), 1); ++perm) {
        if (stage_hashes.contains(HashCombine(program_key, u64{perm})) ||
            (cache_hash != program_key && stage_hashes.contains(HashCombine(cache_hash, u64{perm})))) {
            return true;
        }
    }
    return false;
}

// sotc: a pipeline keeps raw pointers to its programs' Info (Pipeline::GetStages), read by every draw that binds it
// (Liverpool::CaptureBackOp, the rasterizer). A prepared program a pipeline was built with but that no draw bound yet
// (sotc_pre_checked unset) is one of those: a manifest pipeline over a pretranslated program, a prediction, a pipeline
// still on a worker. Evicting it freed the Info under a live pipeline (the next draw of that key read freed memory:
// the PS4 Pro 60 FPS crash in SubmitDraw). Every program a pipeline references stays: by the Info pointers of the built
// ones (the retired fast-linked ones too, a queued BackOp may hold them) and by the stage hashes of every key, which
// also covers the null entries of builds in flight. A manifest stage holds its program (ManifestState::StageRt::program,
// read when its pipelines are scheduled) and the Info its specialization was compared with.
PipelineCache::LiveProgramRefs PipelineCache::CollectLiveProgramRefs() const {
    LiveProgramRefs refs;
    const auto note_pipeline = [&](const Pipeline* pipeline) {
        if (!pipeline) {
            return;
        }
        for (const Shader::Info* info : pipeline->GetStages()) {
            if (info) {
                refs.infos.insert(info);
            }
        }
    };
    refs.infos.reserve(graphics_pipelines.size() * 2 + compute_pipelines.size());
    refs.stage_hashes.reserve(graphics_pipelines.size() * 2);
    for (auto it = graphics_pipelines.begin(); it != graphics_pipelines.end(); ++it) {
        for (const u64 stage_hash : it.key().stage_hashes) {
            if (stage_hash != 0) {
                refs.stage_hashes.insert(stage_hash);
            }
        }
        note_pipeline(it.value().get());
    }
    for (auto it = compute_pipelines.begin(); it != compute_pipelines.end(); ++it) {
        note_pipeline(it.value().get());
    }
    {
        std::scoped_lock lock{g_retired_mutex};
        for (const auto& retired : RetiredLinkedPipelines()) {
            note_pipeline(retired.get());
        }
    }
    if (manifest) {
        for (const auto& st : manifest->stages) {
            if (st.program) {
                refs.programs.insert(st.program);
            }
            if (st.spec.info) {
                refs.infos.insert(st.spec.info);
            }
        }
    }
    return refs;
}

// sotc: SOTC_PRETRANSLATE_STREAM / SOTC_COMPILE_STREAM. Called by the pretranslate hold every 500 ms (force: now). A
// program leaves only when it is in the hold's pack whole (every permutation written: LoadLazyProgram brings it back),
// no draw or dispatch bound it, no translation of it is queued or running, and nothing points at it (see
// CollectLiveProgramRefs). Compute: its prebuilt pipelines (pack cache) were built and left by their workers.
void PipelineCache::PretranslateStreamStep(bool force) {
    using Clock = std::chrono::steady_clock;
    if (!pre_stream_active || !pre_holding) {
        return;
    }
    const auto now = Clock::now();
    if (!force && now - pre_stream_last < std::chrono::milliseconds{500}) {
        return;
    }
    pre_stream_last = now;
    const bool release_cs = SotcNonBlocking::CompileStream() && !SotcNonBlocking::ComputePack();
    // A library job holds pointers to its programs' Info and modules: graphics programs leave only while none runs.
    const bool evict_gfx = SotcNonBlocking::PretranslateStream() && lib_jobs_pending.load(std::memory_order_acquire) == 0;
    if (!release_cs && !evict_gfx) {
        return;
    }
    std::optional<LiveProgramRefs> refs;
    const auto in_pack = [&](u64 hash, const Program& p) {
        const auto it = pre_stream_packed.find(hash);
        return it != pre_stream_packed.end() && !p.modules.empty() && it->second == p.modules.size();
    };
    const auto idle = [&](u64 hash, const Program& p) {
        return p.sotc_pre && !p.sotc_pre_checked && p.pending_specs.empty() && !pending_programs.contains(hash) &&
               !pre_jobs_inflight.contains(hash);
    };
    const auto evict = [&](u64 hash, Program& p) {
        for (auto& m : p.modules) {
            if (m.module) {
                instance.GetDevice().destroyShaderModule(m.module);
                m.module = vk::ShaderModule{};
            }
        }
        lazy_programs[hash] = static_cast<u32>(p.modules.size());
        program_cache.erase(hash);
    };
    u64 released_cs = 0, evicted_gfx = 0;
    if (release_cs) {
        std::vector<u64> programs;
        for (auto it = compute_pipelines.begin(); it != compute_pipelines.end(); ++it) {
            const ComputePipeline& pipe = *it.value();
            if (pipe.sotc_pack && pipe.sotc_worker_done.load(std::memory_order_acquire)) {
                programs.push_back(pipe.sotc_pack_program);
            }
        }
        std::ranges::sort(programs);
        programs.erase(std::unique(programs.begin(), programs.end()), programs.end());
        std::vector<ComputePipelineKey> keys;
        for (const u64 hash : programs) {
            const auto found = program_cache.find(hash);
            if (found == program_cache.end()) {
                continue;
            }
            Program& p = *found.value();
            if (!idle(hash, p) || !in_pack(hash, p)) {
                continue;
            }
            // Every compute pipeline of the program is one of its pack prebuilds, built, left by its worker and
            // accepted by the driver (a refused one is left to the pack step's rule).
            keys.clear();
            bool ok = true;
            const u64 cache_hash = p.info.CacheHash();
            for (size_t perm = 0; perm < p.modules.size() && ok; ++perm) {
                if (cache_hash != hash && compute_pipelines.contains(ComputePipelineKey{HashCombine(cache_hash, perm)})) {
                    ok = false; // a pipeline under the cache hash (the manifest's) points at the program too
                    break;
                }
                const ComputePipelineKey key{HashCombine(hash, perm)};
                const auto pit = compute_pipelines.find(key);
                if (pit == compute_pipelines.end()) {
                    continue;
                }
                const ComputePipeline& pipe = *pit.value();
                ok = pipe.sotc_pack && pipe.sotc_worker_done.load(std::memory_order_acquire) && pipe.IsReady() &&
                     pipe.Handle() != vk::Pipeline{};
                keys.push_back(key);
            }
            if (!ok || keys.empty()) {
                continue;
            }
            if (!refs) {
                refs = CollectLiveProgramRefs();
            }
            if (refs->programs.contains(&p)) {
                continue; // a manifest stage holds it
            }
            for (const ComputePipelineKey key : keys) {
                stream_released_keys.insert(key.value);
                stream_compiled_ids.push_back(PackComputeId(key.value));
                compute_pipelines.erase(key);
                ++stream_released;
                ++released_cs;
            }
            if (stream_released_programs.insert(hash).second) {
                SotcPretranslate::Forget(p.info.pgm_hash); // its shader file's next load requests it again
            }
            evict(hash, p);
        }
        if (released_cs != 0) {
            last_compute_pipeline = nullptr;
            pipe_fast_cs.valid = false;
            refs.reset(); // the compute pipelines changed
        }
    }
    if (evict_gfx) {
        size_t keep = 0;
        for (size_t i = 0; i < pre_stream_candidates.size(); ++i) {
            const u64 hash = pre_stream_candidates[i];
            const auto found = program_cache.find(hash);
            if (found == program_cache.end()) {
                continue; // left (or a compute program the step above released)
            }
            Program& p = *found.value();
            const bool vs = p.info.sw_stage == Shader::SwStage::Vertex && p.info.hw_stage == Shader::HwStage::Vertex;
            const bool ps = p.info.sw_stage == Shader::SwStage::Fragment && p.info.hw_stage == Shader::HwStage::Fragment;
            if ((!vs && !ps) || !p.sotc_pre || p.sotc_pre_checked) {
                continue; // not a pack stage kind, or a draw uses it: not looked at again
            }
            const bool modules = std::ranges::all_of(p.modules, [](const Program::Module& m) { return bool(m.module); });
            if (!idle(hash, p) || !in_pack(hash, p) || !modules) {
                pre_stream_candidates[keep++] = hash;
                continue;
            }
            if (!refs) {
                refs = CollectLiveProgramRefs();
            }
            if (refs->Referenced(hash, p)) {
                pre_stream_candidates[keep++] = hash;
                continue;
            }
            // The first usable program of each kind stays: the pack step's partner (BuildPackLibraries).
            if (!Shader::UsesBindlessSamplers(p.info) &&
                pre_stream_keepers.insert((u32{p.info.variant} << 1) | (vs ? 0u : 1u)).second) {
                continue;
            }
            if (ps) {
                Shader::Backend::Bindings left{};
                p.info.AddBindings(left, Shader::UsesBindlessSamplers(p.info));
                pre_stream_pair_start[hash] = left;
            }
            pre_stream_evicted_gfx.insert(hash);
            evict(hash, p);
            ++evicted_gfx;
        }
        pre_stream_candidates.resize(keep);
        if (evicted_gfx != 0) {
            last_graphics_pipeline = nullptr;
            pipe_fast_gfx.valid = false;
        }
    }
    pre_stream_released_cs += released_cs;
    pre_stream_evicted_count += evicted_gfx;
}

void PipelineCache::StreamRebuildBeforePlay() {
    using Clock = std::chrono::steady_clock;
    if (!SotcNonBlocking::CompileStreamAll() || stream_released_programs.empty()) {
        return;
    }
    // sotc: SOTC_COMPILE_STREAM=2: every released program is loaded back by the workers (8 pack loads at a time) and its
    // pipelines rebuilt from the pack cache (a driver hit), while the loading screen is still up: play starts with
    // every prebuilt pipeline resident (no dispatch builds one).
    const auto start = Clock::now();
    std::vector<u64> programs(stream_released_programs.begin(), stream_released_programs.end());
    std::ranges::sort(programs);
    const u64 rehydrated0 = stream_rehydrated;
    lazy_in_flight_max = 8;
    size_t on_workers = 0, here = 0;
    for (const u64 hash : programs) {
        if (QueueLazyPrefetch(hash)) {
            ++on_workers;
            continue;
        }
        const auto found = program_cache.find(hash);
        if (found != program_cache.end()) {
            // Resident (a pipeline kept it): the destroyed permutations are loaded into it in place.
            if (std::ranges::any_of(found.value()->modules, [](const Program::Module& m) { return !m.module; })) {
                lazy_programs[hash] = static_cast<u32>(found.value()->modules.size());
                LoadLazyProgram(hash);
                lazy_programs.erase(hash);
                last_compute_pipeline = nullptr;
                pipe_fast_cs.valid = false;
            }
        } else if (lazy_programs.contains(hash)) {
            LoadLazyProgram(hash); // not in the pack index: its loose files
        }
        StreamRehydrate(hash);
        ++here;
    }
    SotcStats::prep_total.store(static_cast<u32>(programs.size()), std::memory_order_relaxed);
    SotcStats::prep_done.store(0, std::memory_order_relaxed);
    SotcStats::prep_active.store(true, std::memory_order_release); // the loading screen while they build
    size_t building = 0;
    while (Clock::now() - start < std::chrono::minutes{3}) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            CollectAsyncPipelines();
        }
        ReleaseBuiltComputeModules(false);
        building = 0;
        for (auto it = compute_pipelines.begin(); it != compute_pipelines.end(); ++it) {
            building += it.value()->sotc_rehydrated && !it.value()->IsReady() ? 1 : 0;
        }
        const size_t out = std::ranges::count_if(programs, [this](u64 h) { return stream_released_programs.contains(h); });
        SotcStats::prep_done.store(static_cast<u32>(programs.size() - out), std::memory_order_relaxed);
        if (lazy_prefetch.empty() && lazy_backlog.empty() && building == 0) {
            break;
        }
        std::unique_lock lock{async_mutex};
        translate_done_cv.wait_for(lock, std::chrono::milliseconds{20},
                                   [this] { return async_done_count.load(std::memory_order_relaxed) != 0; });
    }
    SotcStats::prep_active.store(false, std::memory_order_release);
    lazy_in_flight_max = 2;
    ReleaseBuiltComputeModules(true);
    std::fprintf(stderr,
                 "SOTC COMPILE STREAM: rebuilt before play: %zu released programs (%zu loaded on the workers, %zu here), "
                 "%llu pipelines from the pack cache in %.1f s; programs still out %zu, pipelines still building %zu, "
                 "released keys not rebuilt %zu\n",
                 programs.size(), on_workers, here, static_cast<unsigned long long>(stream_rehydrated - rehydrated0),
                 std::chrono::duration<double>(Clock::now() - start).count(), stream_released_programs.size(), building,
                 stream_released_keys.size());
    LogRam("after compute rebuild"); // sotc: SOTC RAM
}

void PipelineCache::StreamRehydrate(u64 program_hash) {
    if (!SotcNonBlocking::CompileStream() || async_workers.empty() || !stream_released_programs.contains(program_hash)) {
        return;
    }
    const auto found = program_cache.find(program_hash);
    if (found == program_cache.end()) {
        return; // not loaded (a read failure): its dispatch loads and builds it (counted as inline then)
    }
    Program& program = *found.value();
    for (size_t perm = 0; perm < program.modules.size(); ++perm) {
        const ComputePipelineKey key{HashCombine(program_hash, perm)};
        if (!stream_released_keys.contains(key.value) || !program.modules[perm].module) {
            continue;
        }
        stream_released_keys.erase(key.value);
        if (compute_pipelines.contains(key)) {
            continue; // a dispatch or the manifest built it meanwhile
        }
        // The ComputePipeline is made here (CP: layouts), its build runs on a worker from the pack cache (a hit).
        ComputePipeline::SerializationSupport sdata{};
        auto pipe = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, layout_cache,
                                                      StageProfile(program.info.variant), PackPipelineCache(), key,
                                                      program.info, program.modules[perm].module, sdata, false, true);
        ComputePipeline* raw = pipe.get();
        raw->sotc_rehydrated = true;
        compute_pipelines.try_emplace(key).first.value() = std::move(pipe);
        last_compute_pipeline = nullptr;
        if (SotcNonBlocking::ComputeRelease()) {
            compute_release_queue.push_back({program_hash, static_cast<u32>(perm), key.value});
        }
        auto job = std::make_unique<AsyncPipelineJob>();
        job->compute = raw;
        {
            std::scoped_lock lock{async_mutex};
            PushJobLocked(JobLane::Compute, std::move(job)); // ahead of graphics: a dispatch may come soon
        }
        async_cv.notify_one();
        ++stream_rehydrated;
    }
    stream_released_programs.erase(program_hash);
    StreamLog(false);
}

void PipelineCache::StreamLog(bool force) {
    const auto now = std::chrono::steady_clock::now();
    static u64 last_sum = ~u64{0};
    const u64 sum = stream_released + stream_rehydrated + stream_inline + stream_late;
    if (!force && (sum == last_sum || now - stream_logged < std::chrono::seconds{5})) {
        return;
    }
    stream_logged = now;
    last_sum = sum;
    std::fprintf(stderr,
                 "SOTC COMPILE STREAM: released %llu compute pipelines to the pack cache (%zu keys, %zu programs still "
                 "out); rebuilt on workers %llu; built by a dispatch first (inline) %llu; dispatched before their "
                 "rebuild was ready %llu\n",
                 static_cast<unsigned long long>(stream_released), stream_released_keys.size(),
                 stream_released_programs.size(), static_cast<unsigned long long>(stream_rehydrated),
                 static_cast<unsigned long long>(stream_inline), static_cast<unsigned long long>(stream_late));
}

vk::PipelineCache PipelineCache::PackPipelineCache() const {
    return pack_cache ? *pack_cache : *pipeline_cache;
}

bool PipelineCache::PackComputeListed(u64 compute_key_value) {
    if (!pack_listed_set_loaded) {
        // Once: the list of the loaded pack cache, when it describes it (else nothing is listed until this session's
        // pack publishes one, BuildPackLibraries).
        pack_listed_set_loaded = true;
        if (PackLibraryListMatches()) {
            ReadPackLibList(PackLibIdentity(instance), loaded_driver_cache_hash, pack_listed_set);
        }
    }
    return !pack_listed_set.empty() && pack_listed_set.contains(PackComputeId(compute_key_value));
}

// sotc: SOTC_GPL_PACK_LEAN. Saver thread: the driver cache first, then the list bound to the written cache's hash, so
// the list never names a library the saved cache lacks.
void PipelineCache::RunPackPublish() {
    std::vector<u64> ids;
    std::vector<u8> header;
    {
        std::scoped_lock lk{driver_cache_cv_mutex};
        ids.swap(pack_publish_ids);
        header.swap(pack_publish_header);
    }
    const auto start = std::chrono::steady_clock::now();
    size_t written_size = 0;
    u64 written_hash = 0;
    // sotc: SOTC_PLAY_QUIET: the libraries live in the pack cache; without the split, in the driver cache.
    const u64 builds_before = GplLibraryCache::LibraryBuilds();
    bool ok = header.size() == sizeof(PackLibHeader) &&
              (pack_cache ? SavePackCache(&written_size, &written_hash)
                          : SaveDriverCache(true, &written_size, &written_hash));
    const auto cache_end = std::chrono::steady_clock::now();
    if (ok) {
        PackLibHeader h;
        std::memcpy(&h, header.data(), sizeof h);
        h.cache_hash = written_hash;
        ok = WritePackLibList(h, ids);
        pack_cache_published = builds_before;
        std::scoped_lock lk{driver_cache_cv_mutex};
        pack_listed_ids = ids;
    }
    std::fprintf(stderr, "SOTC GPL PACK LIBS: publish on the saver thread: driver cache %zu MB in %.1f s, list of %zu "
                         "libraries %s (%.2f s)\n",
                 written_size >> 20, std::chrono::duration<double>(cache_end - start).count(), ids.size(),
                 ok ? "written" : "not written", std::chrono::duration<double>(std::chrono::steady_clock::now() - cache_end).count());
    {
        std::scoped_lock lk{driver_cache_cv_mutex};
        pack_publish_state.store(ok ? 2 : 3, std::memory_order_release);
    }
    pack_publish_cv.notify_all();
}

// sotc: SOTC_PACK_REFERENCED. Command processor: the late libraries the workers finished are counted (built before or
// after the first draw of their program) and listed; their ids join the list that the exit publication writes (the
// pack cache holds their code, FinalPackPublish).
void PipelineCache::DrainPackRef() {
    std::vector<PackRef::Done> done;
    {
        std::scoped_lock lock{pack_mutex};
        done.swap(pack_ref.done);
        pack_ref.done_count.store(0, std::memory_order_release);
    }
    std::vector<u64> append;
    for (const auto& d : done) {
        if (!d.ok) {
            continue;
        }
        ++pack_ref.built;
        const auto f = pack_ref.watch.find(d.program);
        if (f == pack_ref.watch.end() || !f->second) {
            ++pack_ref.before;
        } else {
            ++pack_ref.after;
        }
        if (pack_listed_set.contains(d.id) || !pack_ref.listed_late.insert(d.id).second) {
            continue;
        }
        pack_listed_set.insert(d.id);
        append.push_back(d.id);
    }
    if (!append.empty()) {
        std::scoped_lock lk{driver_cache_cv_mutex};
        pack_listed_ids.insert(pack_listed_ids.end(), append.begin(), append.end());
    }
}

void PipelineCache::NotePackRefDraw() {
    for (const u32 stage : {u32(Shader::SwStage::Vertex), u32(Shader::SwStage::Fragment)}) {
        const auto* info = infos[stage];
        if (!info) {
            continue;
        }
        const auto f = pack_ref.watch.find(info->pgm_hash);
        if (f != pack_ref.watch.end() && !f->second) {
            f->second = true;
            --pack_ref.undrawn;
        }
    }
}

void PipelineCache::PackReferencedLog() {
    std::fprintf(stderr,
                 "SOTC PACK REFERENCED: materials %llu, programs %llu (of %llu in the pack), libraries %llu compiled "
                 "at load in %.1f s; late: queued %llu, built %llu, before first draw %llu, after %llu\n",
                 static_cast<unsigned long long>(SotcPipePredict::Stats().queued),
                 static_cast<unsigned long long>(pack_ref.programs),
                 static_cast<unsigned long long>(pack_ref.programs_all),
                 static_cast<unsigned long long>(pack_ref.libs_at_load), pack_ref.load_seconds,
                 static_cast<unsigned long long>(pack_ref.queued), static_cast<unsigned long long>(pack_ref.built),
                 static_cast<unsigned long long>(pack_ref.before), static_cast<unsigned long long>(pack_ref.after));
}

// sotc: SOTC_PLAY_QUIET. At exit (the saver joined): the pack cache is written again when libraries were created
// since its publication (a draw's urgent ones, predicted ones of programs the pack did not know), and the list is
// written after it, with the new hash, so the next launch accepts it ("already compiled (listed)").
void PipelineCache::FinalPackPublish() {
    if (pack_ref.pack_done) { // sotc: SOTC_PACK_REFERENCED: the late libraries, listed before the list is written
        DrainPackRef();
        PackReferencedLog();
    }
    if (!pack_cache) {
        return;
    }
    if (pack_publish_state.load(std::memory_order_acquire) == 1) {
        RunPackPublish(); // the saver did not get to it
        return;
    }
    const u64 builds = GplLibraryCache::LibraryBuilds();
    if (builds == pack_cache_published) {
        return;
    }
    size_t written_size = 0;
    u64 written_hash = 0;
    if (!SavePackCache(&written_size, &written_hash)) {
        return;
    }
    std::vector<u64> ids;
    {
        std::scoped_lock lk{driver_cache_cv_mutex};
        ids = pack_listed_ids;
    }
    bool list_ok = false;
    if (!ids.empty()) {
        PackLibHeader h = PackLibIdentity(instance);
        h.cache_hash = written_hash;
        list_ok = WritePackLibList(h, ids);
    }
    const u64 since = builds - std::min(builds, pack_cache_published);
    pack_cache_published = builds;
    std::fprintf(stderr, "SOTC GPL PACK LIBS: pack cache written at exit (%zu MB, %llu libraries created since its "
                         "publication), list of %zu libraries %s\n",
                 written_size >> 20, static_cast<unsigned long long>(since), ids.size(),
                 list_ok ? "rewritten" : "not written");
}

// sotc: SOTC_GPL_PACK_LEAN. Workers added for the pack compile, up to the hardware thread count (the draw and command
// processor threads idle at the loading screen), and their stop (they leave at the next idle moment; a job they run
// finishes first; nothing is drained).
void PipelineCache::StartExtraWorkers() {
    size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    // sotc: SOTC_COMPILE_THREADS: no extra worker beyond the hold cap (on 8C/16T: 8 < 12 workers, so none start).
    if (CompileThreadCap() != 0) {
        hw = std::min<size_t>(hw, CompileThreadCap());
    }
    if (hw <= async_workers.size() || !extra_workers.empty()) {
        return;
    }
    extra_workers_quit.store(false, std::memory_order_release);
    for (size_t i = async_workers.size(); i < hw; ++i) {
        extra_workers.emplace_back([this](std::stop_token token) { AsyncPipelineWorker(token, false, true); });
    }
}

void PipelineCache::StopExtraWorkers() {
    if (extra_workers.empty()) {
        return;
    }
    {
        std::scoped_lock lock{async_mutex};
        extra_workers_quit.store(true, std::memory_order_release);
    }
    async_cv.notify_all();
    urgent_cv.notify_all();
    for (auto& worker : extra_workers) {
        worker.join();
    }
    extra_workers.clear();
    extra_workers_quit.store(false, std::memory_order_release);
}

void PipelineCache::MaybeHoldForPretranslate() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point session_start = Clock::now();
    if (pre_hold_done || SotcPretranslate::Precompile()) {
        return;
    }
    if (Clock::now() - session_start > std::chrono::minutes{2}) {
        pre_hold_done = true; // past the boot: the backlog is fed slowly, never held for
        return;
    }
    // sotc: warm SPIR-V with a missing/rejected/replaced driver cache still needs its GPL pack.
    // This work must not depend on accumulating 300 new translations.
    if (!pack_list_checked && warmup_done.load(std::memory_order_acquire)) {
        pack_list_checked = true;
        pack_list_valid = PackLibraryListMatches();
    }
    const bool restore_pack = pack_list_checked && !pack_list_valid && SotcNonBlocking::GplPackLibs() &&
                              DriverCacheEnabled() && !lazy_programs.empty() &&
                              SotcPipeManifest::LastProgramNs() != 0;
    if (pre_backlog.size() + pre_in_flight < 300 && !restore_pack) {
        return;
    }
    if (SotcNonBlocking::PlayQuiet() && !BbOverlay::LoadingShown()) {
        // sotc: SOTC_PLAY_QUIET: the hold starts only while the loading screen is shown (the game not flipping); until
        // then the backlog is fed in the background. After the 2-minute boot window it never runs.
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "SOTC PRETRANSLATE: hold deferred: the game is drawing (waiting for a loading screen)\n");
        }
        return;
    }
    pre_hold_done = true;
    pre_holding = true;
    SetCompileHold(true, "pretranslate hold start"); // sotc: SOTC_COMPILE_THREADS, SOTC RAM
    // sotc: SOTC_PRETRANSLATE_STREAM / SOTC_COMPILE_STREAM: programs leave memory during the hold (not when it restores
    // the GPL pack's modules: those are all loaded for it).
    pre_stream_active = (SotcNonBlocking::PretranslateStream() ||
                         (SotcNonBlocking::CompileStream() && !SotcNonBlocking::ComputePack())) &&
                        !restore_pack && !async_workers.empty();
    pre_ram_decile = 0;
    const auto start = Clock::now();
    std::fprintf(stderr, "SOTC PRETRANSLATE: preparing the game's shaders (%zu translations) before play\n",
                 pre_backlog.size() + pre_in_flight);
    last_graphics_pipeline = nullptr; // installs may change the caches under the draw being looked up
    SotcStats::prep_start_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch()).count(),
        std::memory_order_relaxed);
    SotcStats::prep_active.store(true, std::memory_order_release);
    for (;;) {
        DrainPretranslate();
        FeedPretranslate();
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            CollectAsyncPipelines();
        }
        PretranslateStreamStep(false); // sotc: SOTC_PRETRANSLATE_STREAM / SOTC_COMPILE_STREAM
        const u64 queued = SotcPretranslate::JobsQueued();
        const u64 done = SotcPretranslate::JobsDone();
        SotcStats::prep_total.store(static_cast<u32>(queued), std::memory_order_relaxed);
        SotcStats::prep_done.store(static_cast<u32>(done), std::memory_order_relaxed);
        // sotc: SOTC RAM: one line per tenth of the translations done (of those queued so far, which grow).
        if (queued != 0) {
            const int decile = static_cast<int>(std::min<u64>(done * 10 / queued, 10));
            if (decile > pre_ram_decile) {
                pre_ram_decile = decile;
                char where[96];
                std::snprintf(where, sizeof where, "pretranslate %d%% (%llu of %llu)", decile * 10,
                              static_cast<unsigned long long>(done), static_cast<unsigned long long>(queued));
                LogRam(where);
            }
        }
        // The cache files (one meta and one binary per variant, ~38,000 for the game) are written before play: a
        // write queue left to drain during play slowed every thread by ~40% (file creation, antivirus scans).
        const u32 writes = Storage::DataBase::Instance().PendingWrites();
        SotcStats::prep_writes.store(writes, std::memory_order_relaxed);
        const auto now = Clock::now();
        const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        const bool idle = pre_backlog.empty() && pre_in_flight == 0 && pre_builds_pending.load() == 0 &&
                          !SotcPretranslate::Pending() &&
                          pre_deferred.empty() && now_ns - SotcPretranslate::LastRequestNs() > 3'000'000'000LL &&
                          writes < 64;
        if (idle || now - start > std::chrono::minutes{45}) {
            break;
        }
        static bool index_saved = false;
        if (!index_saved && pre_backlog.empty() && pre_in_flight == 0 && queued == done) {
            index_saved = true;
            SaveShaderIndex(); // written with the other files, so the queue the hold waits for holds it too
        }
        std::unique_lock lock{async_mutex};
        translate_done_cv.wait_for(lock, std::chrono::milliseconds{50},
                                   [this] { return async_done_count.load(std::memory_order_relaxed) != 0; });
    }
    PretranslateStreamStep(true); // sotc: the last translations' programs, before the pack step
    if (pre_stream_active) {
        std::fprintf(stderr,
                     "SOTC PRETRANSLATE STREAM: during the translations %llu programs left memory for the index "
                     "(graphics %llu; compute pipelines released %llu), %zu programs kept as pack partners\n",
                     static_cast<unsigned long long>(pre_stream_evicted_count + stream_released_programs.size()),
                     static_cast<unsigned long long>(pre_stream_evicted_count),
                     static_cast<unsigned long long>(pre_stream_released_cs), pre_stream_keepers.size());
        LogRam("translations done");
    }
    if (restore_pack) {
        // sotc: these cached modules were never needed by this session's translations, but are required
        // to restore the driver pack. Keep existing/drawn programs intact; newly loaded ones may leave
        // memory at the normal eviction below, after every library job has finished.
        std::vector<u64> hashes;
        hashes.reserve(lazy_programs.size());
        for (const auto& [hash, count] : lazy_programs) {
            hashes.push_back(hash);
        }
        size_t loaded = 0;
        for (const u64 hash : hashes) {
            const bool resident = program_cache.contains(hash);
            loaded += LoadLazyProgram(hash);
            if (!resident) {
                if (const auto found = program_cache.find(hash); found != program_cache.end()) {
                    found.value()->sotc_pre = true;
                }
            }
        }
        std::fprintf(stderr, "SOTC GPL PACK LIBS: restored modules for %zu of %zu cached programs before play\n",
                     loaded, hashes.size());
    }
    // sotc: SOTC_NB_GPL: the libraries of the full-screen passes earlier sessions met, while the screen is still up.
    if (SotcNonBlocking::Gpl()) {
        BuildTinyLibraries();
    }
    // sotc: SOTC_GPL_PACK_LIBS: the stage libraries of every translated program, compiled into the driver cache while
    // the screen is still up and the modules are resident (the eviction below destroys them).
    if (!BuildPackLibraries()) {
        std::fprintf(stderr, "SOTC PRETRANSLATE: GPL pack incomplete; remaining first-use builds are not covered\n");
        // sotc: the eviction below already waits for library jobs and preserves all modules if jobs remain.
        // A finished but failed/unlinkable pack must not pin every shader module for the whole session.
    }
    pre_holding = false;
    SetCompileHold(false, "pretranslate hold end"); // sotc: SOTC_COMPILE_THREADS, SOTC RAM
    PrePackFinish(); // the variants of the hold, in one file; read back for their first draws
    // sotc: the driver's pipelines of the hold are written now, not at the 60 s timer. SOTC_NB_IO: by the saver
    // thread, so the hold screen does not wait for it (17.6 s on the CP in the P0 baseline).
    if (SotcNonBlocking::Io() || SotcNonBlocking::PlayQuiet()) {
        RequestDriverCacheSave(); // sotc: SOTC_PLAY_QUIET: never on the command processor
    } else {
        SaveDriverCache();
    }
    SotcStats::prep_active.store(false, std::memory_order_release); // the screen goes away
    SaveShaderIndex();
    // The programs no draw used yet leave memory: their modules are destroyed and they go back to the index on disk,
    // so this session runs as the next ones (LoadLazyProgram at the first draw). With ~19,000 programs and their
    // modules resident, every thread of the port ran ~40% slower.
    // sotc: SOTC_GPL_PREDICT: a library job holds pointers to its programs' Info and modules, so none may be queued or
    // running across the eviction (the command processor queues none while it holds here); after 30 s, no eviction.
    const auto lib_wait_start = Clock::now();
    while (lib_jobs_pending.load(std::memory_order_relaxed) != 0 &&
           Clock::now() - lib_wait_start < std::chrono::seconds{30}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    // sotc: SOTC_COMPUTE_RELEASE: the modules of the hold's built compute pipelines leave memory first.
    ReleaseBuiltComputeModules(true);
    // sotc: library jobs hold pointers to graphics programs only, so with SOTC_COMPUTE_RELEASE the compute programs
    // are evicted even when the wait above timed out (a pack-touched program whose modules are gone must not stay).
    const bool libs_idle = lib_jobs_pending.load(std::memory_order_relaxed) == 0;
    if (libs_idle || SotcNonBlocking::ComputeRelease()) {
        // sotc: every program a pipeline or a manifest stage points at stays (CollectLiveProgramRefs).
        const LiveProgramRefs refs = CollectLiveProgramRefs();
        const auto referenced = [&refs](u64 program_key, const Program& program) {
            return refs.Referenced(program_key, program);
        };
        size_t evicted = 0, evicted_compute = 0, kept_referenced = 0;
        std::vector<u64> restore;
        for (auto it = program_cache.begin(); it != program_cache.end();) {
            Program& program = *it.value();
            // sotc: a compute pipeline keeps a pointer to its program's Info, so a compute program stays while one
            // exists. It leaves when the compute pack destroyed its pipelines (SOTC_COMPUTE_PACK, SOTC_COMPILE_STREAM:
            // pack_touched) or, with SOTC_COMPUTE_RELEASE, when no compute pipeline names any of its permutations
            // (checked for pack-touched ones too: a manifest pipeline of the program keeps it).
            const bool compute = program.info.sw_stage == Shader::SwStage::Compute;
            const bool release = SotcNonBlocking::ComputeRelease();
            const bool compute_free =
                compute && (pack_touched.contains(it.key()) || release) &&
                (!release || !ProgramHasComputePipeline(it.key(), program));
            if (!program.sotc_pre || program.sotc_pre_checked || !program.pending_specs.empty() ||
                (compute && !compute_free) || (!compute && !libs_idle)) {
                ++it;
                continue;
            }
            if (referenced(it.key(), program)) {
                ++kept_referenced; // sotc: a pipeline holds its Info (see above)
                // The lean pack may have destroyed some of its modules (it expected the eviction to send the program
                // back to the index): they are loaded again below, so a new key over the program can still build.
                if (std::ranges::any_of(program.modules, [](const Program::Module& m) { return !m.Present(); })) {
                    restore.push_back(it.key());
                }
                ++it;
                continue;
            }
            evicted_compute += compute ? 1 : 0;
            for (auto& module : program.modules) {
                if (module.module) {
                    instance.GetDevice().destroyShaderModule(module.module);
                }
            }
            lazy_programs[it.key()] = static_cast<u32>(program.modules.size());
            it = program_cache.erase(it);
            ++evicted;
        }
        // sotc: the destroyed modules of the kept programs, from the hold's pack (written above) or the loose files.
        // LoadLazyProgram loads only the permutations that are not present, into the program in place (its Info stays).
        size_t restored = 0;
        for (const u64 hash : restore) {
            const auto found = program_cache.find(hash);
            if (found == program_cache.end()) {
                continue;
            }
            lazy_programs[hash] = static_cast<u32>(found.value()->modules.size());
            restored += LoadLazyProgram(hash) ? 1 : 0;
            lazy_programs.erase(hash);
        }
        if (evicted != 0 || !restore.empty()) {
            // sotc: no lookup record may keep a pointer into an erased program or a replaced permutation.
            last_graphics_pipeline = nullptr;
            last_compute_pipeline = nullptr;
            pipe_fast_gfx.valid = false;
            pipe_fast_cs.valid = false;
        }
        std::fprintf(stderr,
                     "SOTC PRETRANSLATE: %zu prepared programs left for their first draw (on disk; compute %zu); %zu "
                     "kept, a pipeline references them (modules loaded again for %zu of %zu)\n",
                     evicted, evicted_compute, kept_referenced, restored, restore.size());
    }
    LogRam("after pretranslate eviction"); // sotc: SOTC RAM
    StreamRebuildBeforePlay(); // sotc: SOTC_COMPILE_STREAM=2
    pre_stream_active = false;
    pre_stream_candidates.clear();
    pre_stream_candidates.shrink_to_fit();
    pre_stream_packed.clear();
    std::fprintf(stderr, "SOTC PRETRANSLATE: shaders prepared in %.1f s (%llu translations)\n",
                 std::chrono::duration<double>(Clock::now() - start).count(),
                 static_cast<unsigned long long>(SotcPretranslate::JobsDone()));
}

// sotc: SOTC_COMPUTE_SPECFREE. A dispatch needs a compute permutation its program does not have. Counted against the
// program's first permutation (the prebuilt one when the program came from a prebuild) by the first key part that
// differs: image dimension, image integer type, a written formatted buffer, an atomic image, anything else. A program
// without a prebuild (a dispatch translated it first) counts as other. The first 32 are logged with the reason.
void PipelineCache::NoteSpecFreeFallback(const Program& program, const Shader::StageSpecialization& spec, u64 hash) {
    const char* reason = "other";
    std::atomic<u64>* counter = &g_specfree.fallback_other;
    char detail[96] = "";
    if (!program.modules.empty() && program.sotc_pre) {
        // sotc: the int/float variants (InstallComputePrebuild): compared with the permutation whose image integer
        // bits are the dispatch's, so "int" means no prebuilt combination has them.
        const auto same_int = [&spec](const Program::Module& m) {
            if (m.spec.images.size() != spec.images.size()) {
                return false;
            }
            for (size_t i = 0; i < spec.images.size(); ++i) {
                if (m.spec.images[i].is_integer != spec.images[i].is_integer) {
                    return false;
                }
            }
            return true;
        };
        const auto ref = std::ranges::find_if(program.modules, same_int);
        const auto& made = (ref != program.modules.end() ? *ref : program.modules[0]).spec;
        const auto& info = program.info;
        for (size_t i = 0; i < spec.images.size() && i < made.images.size() && detail[0] == '\0'; ++i) {
            const auto& a = spec.images[i];
            const auto& b = made.images[i];
            if (a.type != b.type) {
                reason = "dim";
                counter = &g_specfree.fallback_dim;
                std::snprintf(detail, sizeof detail, " image %zu type %u (prebuilt %u)", i, u32(a.type), u32(b.type));
            } else if (a.is_integer != b.is_integer) {
                const bool capped = program.sotc_ambiguous > Shader::kSpecFreeMaxAmbiguous;
                reason = capped ? "int (over cap)" : "int";
                counter = &g_specfree.fallback_int;
                if (capped) {
                    g_specfree.fallback_int_capped.fetch_add(1, std::memory_order_relaxed);
                }
                std::snprintf(detail, sizeof detail, " image %zu integer %d (%zu perms, %u ambiguous)", i,
                              int(a.is_integer), program.modules.size(), u32{program.sotc_ambiguous});
            } else if (!(a == b) && i < info.images.size() && info.images[i].is_atomic) {
                reason = "atomic";
                counter = &g_specfree.fallback_atomic;
                std::snprintf(detail, sizeof detail, " image %zu", i);
            } else if (!(a == b)) {
                std::snprintf(detail, sizeof detail, " image %zu conv %u bindings %u (prebuilt %u, %u)", i,
                              u32(a.num_conversion), a.num_bindings, u32(b.num_conversion), b.num_bindings);
            }
        }
        for (size_t i = 0; i < spec.buffers.size() && i < made.buffers.size() && detail[0] == '\0'; ++i) {
            if (spec.buffers[i] == made.buffers[i]) {
                continue;
            }
            if (i < info.buffers.size() && info.buffers[i].is_written && spec.buffers[i].is_formatted) {
                reason = "written-fmt";
                counter = &g_specfree.fallback_written_fmt;
            }
            std::snprintf(detail, sizeof detail, " buffer %zu format %u/%u stride %u swizzle %u", i,
                          u32(spec.buffers[i].data_format), u32(spec.buffers[i].num_format),
                          u32(spec.buffers[i].stride), u32(spec.buffers[i].swizzle_enable));
        }
    } else {
        std::snprintf(detail, sizeof detail, " (no prebuilt permutation)");
    }
    counter->fetch_add(1, std::memory_order_relaxed);
    if (g_specfree.logged.fetch_add(1, std::memory_order_relaxed) < 32) {
        std::fprintf(stderr, "SOTC COMPUTE SPECFREE fallback %#llx: %s%s\n", static_cast<unsigned long long>(hash),
                     reason, detail);
    }
}

PipelineCache::Result PipelineCache::GetProgram(HwStage hw_stage, SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding,
                                                bool async_ok) {
    // sotc: SOTC_ASYNC_COMPUTE_XLATE: a stage that may not wait for a worker but whose shader
    // is already on one (queued ahead by the command buffer scan) waits for that translation
    // instead of making a second one here. Compute only: installing finished jobs in the middle
    // of a graphics key would move the modules a previous stage's fetch_shader points into.
    if (hw_stage == HwStage::Compute && !async_ok && compute_prefetch &&
        pending_programs.contains(params.hash)) {
        WaitPendingTranslation(params.hash);
    }
    if (SotcPretranslate::On() && (SotcPretranslate::Pending() || !pre_deferred.empty())) {
        DrainPretranslate(); // sotc: SOTC_PRETRANSLATE
    }
    if (SotcPretranslate::On() && !pre_hold_done) {
        MaybeHoldForPretranslate(); // sotc: the first session's shaders, prepared on the loading screen
    }
    if (SotcPretranslate::Precompile()) {
        FeedPretranslate();
        PrecompileCheck(); // sotc: SOTC_PRECOMPILE (progress, exit when done)
    }
    // sotc: a program of the shader cache index that no warm-up pipeline loaded: its permutations from the disk.
    if (!lazy_programs.empty() && lazy_programs.contains(params.hash)) {
        LoadLazyProgram(params.hash);
    }
    // sotc: the index grows with every translation; it is written every 10 s (and at the end of a precompile run).
    {
        static auto last_index_save = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_index_save > std::chrono::seconds{10}) {
            last_index_save = now;
            SaveShaderIndex();
        }
    }
    // sotc: copied only where CompileModule needs a mutable one (it was copied on every call).
    const auto& rt = BuildRuntimeInfo(hw_stage, sw_stage);
    if (sw_stage == SwStage::Fragment) {
        // sotc: SOTC_PS_INPUT_NORMALIZE, before the key and the translation see the inputs (see above).
        auto& fs_rt = runtime_infos[u32(sw_stage)];
        if (const u32 changed = NormalizeFlatInputs(fs_rt, PassthroughSensitiveInputs(params.hash, params.code))) {
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::fprintf(stderr, "SOTC PS INPUT NORMALIZE: program %#llx, %u flat inputs keyed as "
                             "passthrough\n", static_cast<unsigned long long>(params.hash), changed);
            }
        }
    }
    if (async_ok && !program_cache.contains(params.hash) &&
        !in_place_programs.contains(params.hash)) {
        if (pending_programs.insert(params.hash).second) {
            if (sw_stage == SwStage::Compute && Shader::ComputeSpecFree()) {
                g_specfree.not_prebuilt.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_COMPUTE_SPECFREE
            }
            CacheDiagNoteProgram(params.hash);
            QueueAsyncTranslate(hw_stage, sw_stage, params, rt, binding, nullptr, nullptr, 0);
        } else if (pre_pending.erase(params.hash) != 0) {
            // sotc: SOTC_PRETRANSLATE. The pre-translation waits behind every other job: the draw's own translation
            // goes ahead of it (whichever installs first is kept).
            SotcPretranslate::NoteSuperseded(sw_stage == SwStage::Vertex ? 1 : 0);
            CacheDiagNoteProgram(params.hash);
            QueueAsyncTranslate(hw_stage, sw_stage, params, rt, binding, nullptr, nullptr, 0);
        }
        translation_pending = true;
        return {nullptr, vk::ShaderModule{}, 0};
    }
    // sotc: SOTC_SHADER_ADDR (table mode): a compute program the lookup lacks is translated on a worker (the CP waits),
    // not inline here. Before the program cache is read: the wait installs finished jobs (the map may move).
    if (sw_stage == SwStage::Compute && !async_ok && profile.shader_addr != 0 &&
        !program_cache.contains(params.hash) && !in_place_programs.contains(params.hash) &&
        TranslateComputeOnWorker(params, rt, binding, nullptr, nullptr, 0) && program_cache.contains(params.hash)) {
        if (Shader::ComputeSpecFree()) {
            g_specfree.not_prebuilt.fetch_add(1, std::memory_order_relaxed); // sotc: first translated by a dispatch
        }
    }
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        if (sw_stage == SwStage::Compute && Shader::ComputeSpecFree()) {
            g_specfree.not_prebuilt.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_COMPUTE_SPECFREE
        }
        if (sw_stage == SwStage::Compute && profile.shader_addr != 0) {
            g_specfree.cp_inline.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_SHADER_ADDR, still on the CP
        }
        CacheDiagNoteProgram(params.hash);
        it_pgm.value() = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto& program = it_pgm.value();
        auto start = binding;
        auto runtime_info = rt;
        const auto module = CompileModule(program->info, runtime_info, params.code, 0, binding);
        auto spec = Shader::StageSpecialization(program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        program->AddPermut(module, std::move(spec));
        if (auto& fetch = program->modules[0].spec.fetch_shader_data; !fetch.Empty()) {
            fetch_shader = &fetch;
        }
        return std::make_tuple(&program->info, module, perm_hash);
    }

    auto& program = it_pgm.value();
    auto& info = program->info;
    info.PgmBase() = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.UserData() = params.user_data;
    if (PipeDirtyMode() == 0) {
        info.RefreshFlatBuf();
    } else {
        // sotc: SOTC_PIPE_DIRTY=1/2 counts what the refresh does. The refresh still runs in full:
        // the flat buffer also holds SRT and sharp table words read from guest memory, which no
        // register write announces, so a skip needs the memory generation (DRAW-THREAD-DESIGN V1).
        thread_local std::vector<u32> flat_before;
        flat_before.assign(info.FlatBuf().begin(), info.FlatBuf().end());
        info.RefreshFlatBuf();
        const auto& flat_now = info.FlatBuf();
        g_pipe_dirty.flat_refresh.fetch_add(1, std::memory_order_relaxed);
        g_pipe_dirty.flat_words.fetch_add(flat_now.size(), std::memory_order_relaxed);
        if (std::ranges::equal(flat_before, flat_now)) {
            g_pipe_dirty.flat_unchanged.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // sotc: SOTC_PIPE_MEMO=1/2. The flattened buffer, the user data and the fetch code are the
    // memo's inputs. A hit skips the specialization and the search below; the bindings, the fetch
    // shader and the returned values are set the same way. Entries are only stored from this path
    // (an existing program), never for a translation still pending. Unset: none of the code below
    // runs; the locals only start empty.
    // sotc: SOTC_SPEC_MEMO (set) replaces the stage memo: the two are not combined.
    const int spec_mode = SpecMemoMode();
    int memo_mode = spec_mode != 0 ? 0 : PipeMemoStageMode();
    if (memo_mode != 0 && !PipeMemoOwnerThread()) {
        memo_mode = 0; // sotc: a call from another thread takes the slow path (counted)
    }
    // sotc: SOTC_SPEC_MEMO. The key of this stage is built in one pass over its sharps (the reads
    // the specialization makes, BuildSpecKey) and compared with the entries of the memo. Unset:
    // none of the code below runs; the locals only start empty.
    const bool spec_probed = spec_mode != 0 && PipeMemoOwnerThread();
    const Shader::Backend::Bindings spec_start = binding; // the specialization starts from this
    u32 spec_hit = kSpecNone;                             // the entry that answered the probe
    u64 spec_key_hash{};
    bool spec_has_tess = false;
    Shader::TessellationDataConstantBuffer spec_tess{};
    if (spec_probed) {
        const auto probe_start = std::chrono::steady_clock::now();
        spec_has_tess = info.sw_stage == SwStage::TessellationControl ||
                        info.sw_stage == SwStage::TessellationEval;
        if (spec_has_tess) {
            info.ReadTessConstantBuffer(spec_tess);
        }
        Shader::BuildSpecKey(t_spec_words, info, rt, t_spec_fetch);
        spec_key_hash = SpecKeyHash(t_spec_words, program.get(), params.hash, hw_stage, sw_stage,
                                    binding);
        spec_hit = SpecMemoFind(spec_memo_state, spec_key_hash, *program, hw_stage, sw_stage,
                                params.hash, binding, rt, spec_has_tess, spec_tess, t_spec_fetch,
                                t_spec_words);
        if (spec_hit != kSpecNone) {
            SpecMemoUnlink(spec_memo_state, spec_hit);
            SpecMemoPushHead(spec_memo_state, spec_hit);
            g_spec_memo.hits.fetch_add(1, std::memory_order_relaxed);
        }
        g_spec_memo.lookups.fetch_add(1, std::memory_order_relaxed);
        g_spec_memo.probe_ns.fetch_add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - probe_start)
                                 .count()),
            std::memory_order_relaxed);
        if (spec_hit != kSpecNone && spec_mode == 1) {
            // The answer of the slow path for an equal key: the same steps as its found branch.
            const SpecMemoEntry& entry = spec_memo_state.entries[spec_hit];
            info.AddBindings(binding, Shader::UsesBindlessSamplers(info));
            auto& found = program->modules[entry.perm];
            if (auto& fetch = found.spec.fetch_shader_data; !fetch.Empty()) {
                fetch_shader = &fetch;
            }
            return std::make_tuple(&program->info, found.module,
                                   HashCombine(params.hash, static_cast<size_t>(entry.perm)));
        }
    }
    Shader::Backend::Bindings memo_start{}; // the specialization starts from this
    Shader::TessellationDataConstantBuffer memo_tess{};
    StageMemo* memo_hit = nullptr;   // the way that answered the lookup
    StageMemo* memo_ways = nullptr;  // the set the lookup used (store target is chosen at store)
    u64 memo_gen = 0;
    bool memo_usable = false;        // false while the runtime memory generation is odd
    bool has_tess = false;
    if (memo_mode != 0) {
        has_tess = info.sw_stage == SwStage::TessellationControl ||
                   info.sw_stage == SwStage::TessellationEval;
        memo_start = binding;
        // Read before the slow path, so a store is tagged with the generation it started from.
        memo_gen = runtime_memory_generation();
        memo_usable = (memo_gen & 1) == 0;
        // Read as the specialization reads it (ForEachSharp / the tessellation runtime info).
        if (has_tess) {
            info.ReadTessConstantBuffer(memo_tess);
        }
        if (stage_memo.empty()) {
            stage_memo.resize(kStageMemoSets * kStageMemoWays);
        }
        memo_ways = &stage_memo[StageMemoSet(params.hash, hw_stage, sw_stage) * kStageMemoWays];
        g_pipe_memo.stage_lookups.fetch_add(1, std::memory_order_relaxed);
        for (size_t w = 0; memo_usable && !memo_hit && w < kStageMemoWays; ++w) {
            if (StageMemoHit(memo_ways[w], *program, hw_stage, sw_stage, params, rt, memo_start,
                             memo_tess, has_tess, memo_gen)) {
                memo_hit = &memo_ways[w];
            }
        }
        if (memo_hit) {
            g_pipe_memo.stage_hits.fetch_add(1, std::memory_order_relaxed);
            memo_hit->stamp = ++stage_memo_tick;
        }
        if (memo_hit && memo_mode == 1) {
            info.AddBindings(binding, Shader::UsesBindlessSamplers(info));
            auto& found = program->modules[memo_hit->perm];
            if (auto& fetch = found.spec.fetch_shader_data; !fetch.Empty()) {
                fetch_shader = &fetch;
            }
            return std::make_tuple(&program->info, found.module,
                                   HashCombine(params.hash, static_cast<size_t>(memo_hit->perm)));
        }
    }

    auto spec = Shader::StageSpecialization(info, rt, profile, binding);
    // sotc: SOTC_SPEC_MEMO. A miss is stored only when the specialization just built packs to the
    // key words of the probe (a guest sharp that changed between the two reads fails this check).
    bool spec_store = false;
    if (spec_probed && spec_hit == kSpecNone) {
        Shader::PackSpecObject(t_spec_check, spec);
        spec_store = t_spec_check == t_spec_words && spec.fetch_shader_data == t_spec_fetch;
        if (spec_store && spec_has_tess) {
            // The constructor read the tessellation constants again: they must be the probe's.
            Shader::TessellationDataConstantBuffer tess_now{};
            info.ReadTessConstantBuffer(tess_now);
            spec_store = std::memcmp(&tess_now, &spec_tess, sizeof(tess_now)) == 0;
        }
        if (!spec_store) {
            g_spec_memo.store_skipped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (program->sotc_pre && !program->sotc_pre_checked && !program->modules.empty()) {
        // sotc: SOTC_PRETRANSLATE: the first draw of a pre-translated program, against the key it was made for.
        program->sotc_pre_checked = true;
        const bool hit = it != program->modules.end();
        if (!hit && sw_stage == SwStage::Fragment) {
            // sotc: the first pixel image misses: each image's key at the draw against the pre-translated one.
            static std::atomic<u32> logged_ps{0};
            const auto& made = program->modules[0].spec;
            if (spec.images != made.images && logged_ps.fetch_add(1, std::memory_order_relaxed) < 32) {
                std::string line;
                for (size_t i = 0; i < spec.images.size() && i < made.images.size(); ++i) {
                    const auto& a = spec.images[i];
                    const auto& b = made.images[i];
                    if (a == b) {
                        continue;
                    }
                    const auto& d = program->info.images[i];
                    char buf[200];
                    std::snprintf(buf, sizeof buf,
                                  " [%zu %s%s%s draw t%u i%d s%d sw%d%d%d%d c%u n%u | made t%u i%d s%d sw%d%d%d%d c%u n%u]", i,
                                  d.is_written ? "W" : "R", d.is_array ? "A" : "", d.is_depth ? "D" : "", u32(a.type),
                                  a.is_integer, a.is_storage, int(a.dst_select.r), int(a.dst_select.g),
                                  int(a.dst_select.b), int(a.dst_select.a), u32(a.num_conversion), a.num_bindings,
                                  u32(b.type), b.is_integer, b.is_storage, int(b.dst_select.r), int(b.dst_select.g),
                                  int(b.dst_select.b), int(b.dst_select.a), u32(b.num_conversion), b.num_bindings);
                    line += buf;
                }
                std::fprintf(stderr, "SOTC PRETRANSLATE ps image miss %#llx:%s\n",
                             static_cast<unsigned long long>(params.hash), line.c_str());
            }
        }
        if (!hit && sw_stage == SwStage::Vertex && spec.start != program->modules[0].spec.start) {
            static std::atomic<u32> logged_start{0};
            if (logged_start.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::fprintf(stderr, "SOTC PRETRANSLATE vs start miss %#llx: draw %u/%u made %u/%u\n",
                             static_cast<unsigned long long>(params.hash), spec.start.unified, spec.start.buffer,
                             program->modules[0].spec.start.unified, program->modules[0].spec.start.buffer);
            }
        }
        if (!hit && sw_stage == SwStage::Vertex) {
            // sotc: the first vertex misses, with the draw's software state against the pre-translated one.
            static std::atomic<u32> logged{0};
            const auto& sa = spec.runtime_info.sw.vs;
            const auto& sb = program->modules[0].spec.runtime_info.sw.vs;
            const auto& ha = spec.runtime_info.hw.vs;
            if (logged.fetch_add(1, std::memory_order_relaxed) < 24) {
                std::fprintf(stderr,
                             "SOTC PRETRANSLATE vs miss %#llx: draw step %u/%u vsgpr %u isgpr %u tess %d clipoff %d "
                             "outputs %u; made step %u/%u vsgpr %u isgpr %u tess %d clipoff %d outputs %u\n",
                             static_cast<unsigned long long>(params.hash), sa.step_rate_0, sa.step_rate_1,
                             sa.vertex_sgpr_offset, sa.instance_sgpr_offset, sa.tess_emulated_primitive,
                             ha.clip_disable, ha.num_outputs, sb.step_rate_0, sb.step_rate_1, sb.vertex_sgpr_offset,
                             sb.instance_sgpr_offset, sb.tess_emulated_primitive,
                             program->modules[0].spec.runtime_info.hw.vs.clip_disable,
                             program->modules[0].spec.runtime_info.hw.vs.num_outputs);
            }
        }
        SotcPretranslate::NoteFirstUse(sw_stage == SwStage::Vertex ? 1 : sw_stage == SwStage::Compute ? 2 : 0, hit,
                                       hit ? 0u : PretranslateDiff(spec, program->modules[0].spec));
        if (sw_stage == SwStage::Compute) {
            // sotc: SOTC_PRETRANSLATE_COMPUTE_HINTS: the dispatch's key against the prebuilt one (hint file, miss log).
            SotcPretranslate::NoteComputeFirstUse(params.hash, hit, program->info, spec, program->modules[0].spec);
        }
    }
    if (it == program->modules.end() && sw_stage == SwStage::Compute && Shader::ComputeSpecFree() &&
        std::ranges::find(program->pending_specs, spec) == program->pending_specs.end()) {
        NoteSpecFreeFallback(*program, spec, params.hash); // sotc: SOTC_COMPUTE_SPECFREE
    }
    if (it == program->modules.end() && async_ok && !program->translate_in_place) {
        if (std::ranges::find(program->pending_specs, spec) == program->pending_specs.end()) {
            CacheDiagNotePerm(true);
            QueueAsyncTranslate(hw_stage, sw_stage, params, rt, binding, program.get(), &spec,
                                perm_idx);
            program->pending_specs.push_back(spec);
        }
        translation_pending = true;
        if (memo_hit) { // =2 only: the memo found a permutation the slow path cannot give yet
            g_pipe_memo.mismatch_stage_pending.fetch_add(1, std::memory_order_relaxed);
        }
        return {nullptr, vk::ShaderModule{}, 0};
    }
    if (it == program->modules.end() && sw_stage == SwStage::Compute && !async_ok && profile.shader_addr != 0 &&
        !program->translate_in_place && !t_cs_perm_retry &&
        std::ranges::find(program->pending_specs, spec) == program->pending_specs.end()) {
        // sotc: SOTC_SHADER_ADDR (table mode): the permutation is translated on a worker (the CP waits), then the
        // lookup runs again from its start (the wait installed jobs: `program` may have moved in the map) and finds it.
        // Once: a second miss (the guest tables changed meanwhile, or a stale worker result) is translated inline.
        Program* const owner = program.get();
        if (TranslateComputeOnWorker(params, rt, spec_start, owner, &spec, perm_idx)) {
            binding = spec_start;
            t_cs_perm_retry = true;
            const auto result = GetProgram(hw_stage, sw_stage, params, binding, async_ok);
            t_cs_perm_retry = false;
            return result;
        }
    }
    if (it == program->modules.end()) {
        CacheDiagNotePerm(false);
        if (sw_stage == SwStage::Compute && profile.shader_addr != 0) {
            g_specfree.cp_inline.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_SHADER_ADDR, still on the CP
        }
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        auto runtime_info = rt;
        const auto binding_before = binding;
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);
        // sotc: the program's Info came from a translation with other resources (a SOTC_PRETRANSLATE layout variant
        // whose color outputs, and the flat buffer reads behind them, were eliminated). The rasterizer writes a
        // draw's descriptors from the program's Info, so this draw's own translation replaces it: the permutations
        // made with the old one keep their specialization lists, which no draw specialized from the new Info matches
        // (StageSpecialization ==, list lengths), and the pipelines of the stage are looked up again.
        if (!SameBindingLayout(info, new_info)) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::fprintf(stderr,
                             "SOTC SHADER: program %#llx takes a draw's resources (buffers %zu -> %zu, images %zu -> "
                             "%zu, samplers %zu -> %zu)\n",
                             static_cast<unsigned long long>(params.hash), info.buffers.size(),
                             new_info.buffers.size(), info.images.size(), new_info.images.size(),
                             info.samplers.size(), new_info.samplers.size());
            }
            info = std::move(new_info);
            info.PgmBase() = params.Base();
            info.UserData() = params.user_data;
            info.RefreshFlatBuf();
            spec = Shader::StageSpecialization(info, rt, profile, binding_before);
            program->translate_in_place = false;
            last_graphics_pipeline = nullptr;
            last_compute_pipeline = nullptr;
            new_info = Shader::Info(hw_stage, sw_stage, params); // moved from: the combine below is a no-op
        }
        // sotc: the new permutation's bindless answers must agree with the program's (CombineBindlessCache).
        Shader::CombineBindlessCache(info.bindless, new_info.bindless);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        program->AddPermut(module, std::move(spec));
    } else {
        perm_idx = std::distance(program->modules.begin(), it);
        info.AddBindings(binding, Shader::UsesBindlessSamplers(info));
        module = it->module;
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    if (memo_mode != 0) {
        if (memo_hit && memo_mode == 2) {
            // sotc: =2 only. The memo answer, as mode 1 would return it, against the slow result.
            // Each kind is counted on its own; the slow result is the one returned.
            const auto& memo_module = program->modules[memo_hit->perm];
            if (memo_hit->perm != perm_idx) {
                g_pipe_memo.mismatch_stage_perm.fetch_add(1, std::memory_order_relaxed);
            } else if (memo_module.module != module) {
                g_pipe_memo.mismatch_stage_module.fetch_add(1, std::memory_order_relaxed);
            }
            if (!(memo_module.spec.fetch_shader_data ==
                  program->modules[perm_idx].spec.fetch_shader_data)) {
                g_pipe_memo.mismatch_stage_fetch.fetch_add(1, std::memory_order_relaxed);
            }
            Shader::Backend::Bindings memo_after = memo_start;
            info.AddBindings(memo_after, Shader::UsesBindlessSamplers(info));
            if (!(memo_after == binding)) {
                g_pipe_memo.mismatch_stage_bindings.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (memo_usable) {
            StageMemo& target =
                memo_hit ? *memo_hit
                         : *StageMemoVictim(memo_ways, *program, hw_stage, sw_stage, params.hash);
            StageMemoStore(target, *program, hw_stage, sw_stage, params, rt, memo_start, memo_tess,
                           has_tess, memo_gen, perm_idx, binding);
        }
    }
    if (spec_probed) {
        if (spec_hit != kSpecNone && spec_mode == 2) {
            // sotc: =2 only. The memo answer, as mode 1 would return it, against the slow result
            // (which is the one returned). Each kind is counted on its own.
            const SpecMemoEntry& entry = spec_memo_state.entries[spec_hit];
            const auto& memo_module = program->modules[entry.perm];
            if (entry.perm != perm_idx) {
                g_spec_memo.mismatch_perm.fetch_add(1, std::memory_order_relaxed);
            } else if (memo_module.module != module) {
                g_spec_memo.mismatch_module.fetch_add(1, std::memory_order_relaxed);
            }
            if (!(memo_module.spec.fetch_shader_data ==
                  program->modules[perm_idx].spec.fetch_shader_data)) {
                g_spec_memo.mismatch_fetch.fetch_add(1, std::memory_order_relaxed);
            }
            Shader::Backend::Bindings memo_after = spec_start;
            info.AddBindings(memo_after, Shader::UsesBindlessSamplers(info));
            if (!(memo_after == binding)) {
                g_spec_memo.mismatch_bindings.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (spec_store) {
            SpecMemoStore(spec_memo_state, spec_key_hash, *program, hw_stage, sw_stage,
                          params.hash, spec_start, rt, spec_has_tess, spec_tess, t_spec_fetch,
                          t_spec_words, static_cast<u32>(perm_idx));
            g_spec_memo.stores.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (auto& fetch = program->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }
    return std::make_tuple(&program->info, module, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    last_graphics_pipeline = nullptr; // sotc: pipelines may be erased below
    last_compute_pipeline = nullptr;  // sotc: SOTC_PIPE_MEMO, same reason
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx, std::string_view ext,
                                                              std::span<const u32> generated) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    // sotc: the patch folder is listed once; two filesystem checks per compiled shader cost
    // ~0.34 s over a 200 s attract run with an empty shader cache (7% of the compile time).
    static const std::unordered_set<std::string> patch_files = [&] {
        std::unordered_set<std::string> names;
        std::error_code ec;
        if (!std::filesystem::exists(patch_dir, ec)) {
            std::filesystem::create_directories(patch_dir, ec);
        }
        for (const auto& entry : std::filesystem::directory_iterator(patch_dir, ec)) {
            names.insert(entry.path().filename().string());
        }
        return names;
    }();
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    if (!patch_files.contains(filename)) {
        return {};
    }
    const auto filepath = patch_dir / filename;
    const u64 generated_hash = XXH3_64bits(generated.data(), generated.size_bytes());
    auto base_path = filepath;
    base_path += ".base";
    if (std::filesystem::exists(base_path)) {
        const auto base_file = IOFile{base_path, FileAccessMode::Read};
        std::string text(base_file.GetSize(), '\0');
        base_file.ReadSpan(std::span<char>{text});
        const u64 base_hash = std::strtoull(text.c_str(), nullptr, 16);
        if (base_hash != generated_hash) {
            LOG_WARNING(Loader,
                        "Not applying patch {}: made for a different module ({:#018x}, this GPU "
                        "generated {:#018x})",
                        filename, base_hash, generated_hash);
            return {};
        }
    } else {
        LOG_INFO(Loader, "Patch {} has no .base file; generated module hash {:#018x}", filename,
                 generated_hash);
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
// sotc: writes a report line for the 5 s report: counts since the previous call, for
// SOTC_PIPE_GPL. Prints nothing while the switch is off or the window had no work. One thread only.
void ReportPipeGpl(std::string& out, double seconds) {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_PIPE_GPL");
        return v && v[0] == '1';
    }();
    // sotc: SOTC_GPL_NODROP prints this line and its own (below) too.
    if (!enabled && !SotcNonBlocking::GplNoDrop()) {
        return;
    }
    using L = GplLibraryCache;
    const auto take = [](const std::atomic<u64>& c, u64& prev) {
        const u64 now = c.load(std::memory_order_relaxed);
        const u64 d = now - prev;
        prev = now;
        return d;
    };
    // sotc: SOTC_GPL_NODROP, same window. Printed when a draw found no built pipeline or a boot library job ran.
    if (SotcNonBlocking::GplNoDrop()) {
        static u64 p_links{}, p_lib_draws{}, p_libs{}, p_lib_ns{}, p_refused{}, p_full{}, p_full_ns{}, p_full_unl{},
            p_xlate{}, p_xlate_ns{}, p_drop_x{}, p_drops{}, p_repl{}, p_served{}, p_boot_q{}, p_boot_d{};
        const u64 links = take(g_nodrop.links, p_links);
        const u64 lib_draws = take(g_nodrop.sync_lib_draws, p_lib_draws);
        const u64 libs = take(g_nodrop.sync_libs, p_libs);
        const u64 lib_ns = take(g_nodrop.sync_lib_ns, p_lib_ns);
        const u64 refused = take(g_nodrop.link_refused, p_refused);
        const u64 full = take(g_nodrop.sync_full, p_full);
        const u64 full_ns = take(g_nodrop.sync_full_ns, p_full_ns);
        const u64 full_unl = take(g_nodrop.sync_full_unlinkable, p_full_unl);
        const u64 xlate = take(g_nodrop.sync_xlate, p_xlate);
        const u64 xlate_ns = take(g_nodrop.sync_xlate_ns, p_xlate_ns);
        const u64 drop_x = take(g_nodrop.drop_translate, p_drop_x);
        // Drops at the pending-pipeline sites (expected 0 with the switch on), on top of the translation ones.
        const u64 drops = take(SotcStats::pipeline_pending_drops, p_drops);
        // The pipe gpl line below takes its own deltas of these two (its own previous values).
        const u64 repl = take(g_gpl_counters.replaced, p_repl);
        const u64 served = take(g_gpl_counters.linked_served, p_served);
        const u64 boot_q = take(g_nodrop.boot_libs_queued, p_boot_q);
        const u64 boot_d = take(g_nodrop.boot_libs_done, p_boot_d);
        // sotc: SOTC_GPL_PREDICT / SOTC_GPL_PARALLEL_LIBS / pack translation deltas of the same window; the predicted
        // library totals (built, used by a draw, never used so far) are since the start.
        static u64 p_pslots{}, p_pjobs{}, p_pnotpl{}, p_pnoprog{}, p_ppack{}, p_pprom{}, p_pdrop{}, p_pdefer{}, p_pwait{},
            p_pwait_ns{}, p_ptaken{}, p_urgent{};
        const u64 pslots = take(g_nodrop.pred_slots, p_pslots);
        const u64 pjobs = take(g_nodrop.pred_jobs, p_pjobs);
        const u64 pnotpl = take(g_nodrop.pred_no_template, p_pnotpl);
        const u64 pnoprog = take(g_nodrop.pred_no_program, p_pnoprog);
        const u64 ppack = take(g_nodrop.pred_pack_loads, p_ppack);
        const u64 pprom = take(g_nodrop.pred_promoted, p_pprom);
        const u64 pdrop = take(g_nodrop.pred_dropped, p_pdrop);
        const u64 pdefer = take(g_nodrop.pre_deferred, p_pdefer);
        const u64 pwait = take(g_nodrop.par_waits, p_pwait);
        const u64 pwait_ns = take(g_nodrop.par_wait_ns, p_pwait_ns);
        const u64 ptaken = take(g_nodrop.par_taken, p_ptaken);
        const u64 urgent = take(g_gpl_counters.urgent_built, p_urgent);
        const u64 pbuilt = g_gpl_counters.predicted_built.load(std::memory_order_relaxed);
        const u64 pused = g_gpl_counters.predicted_used.load(std::memory_order_relaxed);
        if (links != 0 || full != 0 || xlate != 0 || drop_x != 0 || drops != 0 || repl != 0 || served != 0 ||
            boot_d != 0 || refused != 0 || pslots != 0 || pjobs != 0 || pwait != 0 || pdefer != 0) {
            char nline[1400];
            std::snprintf(
                nline, sizeof(nline),
                "SOTC gpl nodrop: linked draws %llu (new links %llu, refused %llu), sync stage libraries %llu in %llu "
                "draws (%.1f ms), sync full builds %llu (%.1f ms; not linkable %llu), sync translations %llu "
                "(%.1f ms), linked->optimized %llu, drops %llu (translation pending %llu, pipeline pending %llu); "
                "boot library jobs %llu/%llu; parallel library waits %llu (%.1f ms, libraries %llu, jobs run by the "
                "command processor %llu); predicted: material slots %llu, library jobs queued %llu, libraries built "
                "%llu / used by a draw %llu / never used %llu (since start), waiting %llu (no pass state %llu, program "
                "not loaded %llu, pack loads %llu, pack translations moved up %llu), given up %llu; pack translations "
                "deferred %llu; process private %.0f MB\n",
                static_cast<unsigned long long>(served), static_cast<unsigned long long>(links),
                static_cast<unsigned long long>(refused), static_cast<unsigned long long>(libs),
                static_cast<unsigned long long>(lib_draws), static_cast<double>(lib_ns) / 1e6,
                static_cast<unsigned long long>(full), static_cast<double>(full_ns) / 1e6,
                static_cast<unsigned long long>(full_unl), static_cast<unsigned long long>(xlate),
                static_cast<double>(xlate_ns) / 1e6, static_cast<unsigned long long>(repl),
                static_cast<unsigned long long>(drop_x + drops), static_cast<unsigned long long>(drop_x),
                static_cast<unsigned long long>(drops), static_cast<unsigned long long>(boot_d),
                static_cast<unsigned long long>(boot_q), static_cast<unsigned long long>(pwait),
                static_cast<double>(pwait_ns) / 1e6, static_cast<unsigned long long>(urgent),
                static_cast<unsigned long long>(ptaken), static_cast<unsigned long long>(pslots),
                static_cast<unsigned long long>(pjobs), static_cast<unsigned long long>(pbuilt),
                static_cast<unsigned long long>(pused),
                static_cast<unsigned long long>(pbuilt > pused ? pbuilt - pused : 0),
                static_cast<unsigned long long>(g_nodrop.pred_waiting.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(pnotpl), static_cast<unsigned long long>(pnoprog),
                static_cast<unsigned long long>(ppack), static_cast<unsigned long long>(pprom),
                static_cast<unsigned long long>(pdrop), static_cast<unsigned long long>(pdefer),
                static_cast<double>(SotcPipeManifest::ProcessPrivateBytes()) / 1048576.0);
            out += nline;
            // sotc: SOTC_GPL_DYN_STATE / SOTC_GPL_PACK_LIBS: the libraries built before any draw (boot manifest,
            // predicted materials) and how many a draw's link found, the pack libraries compiled for the driver cache
            // (destroyed), the libraries held now, and the command processor's library waits of this window (target 0).
            const size_t held = static_cast<size_t>(g_gpl_counters.held.load(std::memory_order_relaxed));
            char lline[768];
            std::snprintf(lline, sizeof(lline),
                          "SOTC gpl libraries: shader-only keys %s; built ahead of draws %llu, used by a draw %llu "
                          "(since start); pack compiled for the driver cache %llu; held %zu; command processor "
                          "library waits %llu (%.1f ms), stage libraries built for a draw %llu (%.1f ms); predicted: "
                          "built before the draw %llu, after the draw %llu, evicted %llu, referenced by materials %zu\n",
                          SotcNonBlocking::GplShaderKeys() ? "on" : "off",
                          static_cast<unsigned long long>(g_gpl_counters.prebuilt_built.load(std::memory_order_relaxed)),
                          static_cast<unsigned long long>(g_gpl_counters.prebuilt_used.load(std::memory_order_relaxed)),
                          static_cast<unsigned long long>(g_gpl_counters.pack_discarded.load(std::memory_order_relaxed)),
                          held, static_cast<unsigned long long>(pwait), static_cast<double>(pwait_ns) / 1e6,
                          static_cast<unsigned long long>(libs), static_cast<double>(lib_ns) / 1e6,
                          static_cast<unsigned long long>(pused), // sotc: SOTC_GPL_PREDICT (plan step 2 counters)
                          static_cast<unsigned long long>(g_gpl_counters.predicted_late.load(std::memory_order_relaxed)),
                          static_cast<unsigned long long>(g_gpl_counters.predicted_evicted.load(std::memory_order_relaxed)),
                          GplLibraryCache::Referenced());
            out += lline;
        }
    }
    static u64 p_cold{}, p_unlink{}, p_served{}, p_links{}, p_failed{}, p_link_ns{}, p_repl{};
    static u64 p_built[L::NumParts]{}, p_reused[L::NumParts]{};
    const u64 cold = take(g_gpl_counters.cold_misses, p_cold);
    const u64 unlink = take(g_gpl_counters.unlinkable, p_unlink);
    const u64 served = take(g_gpl_counters.linked_served, p_served);
    const u64 links = take(g_gpl_counters.links, p_links);
    const u64 failed = take(g_gpl_counters.link_failed, p_failed);
    const u64 link_ns = take(g_gpl_counters.link_ns, p_link_ns);
    const u64 repl = take(g_gpl_counters.replaced, p_repl);
    u64 built[L::NumParts]{}, reused[L::NumParts]{};
    for (u32 i = 0; i < L::NumParts; ++i) {
        built[i] = take(g_gpl_counters.lib_built[i], p_built[i]);
        reused[i] = take(g_gpl_counters.lib_reused[i], p_reused[i]);
    }
    if (cold == 0 && served == 0 && repl == 0 && failed == 0) {
        return;
    }
    char line[480];
    std::snprintf(
        line, sizeof(line),
        "SOTC pipe gpl: cold misses %llu (not linkable %llu), linked draws %llu, links %llu "
        "(failed %llu, avg %.2f ms), optimized replacements %llu; libraries built vi %llu pr %llu "
        "fs %llu fo %llu, reused vi %llu pr %llu fs %llu fo %llu\n",
        static_cast<unsigned long long>(cold), static_cast<unsigned long long>(unlink),
        static_cast<unsigned long long>(served), static_cast<unsigned long long>(links),
        static_cast<unsigned long long>(failed),
        links ? static_cast<double>(link_ns) / 1e6 / static_cast<double>(links) : 0.0,
        static_cast<unsigned long long>(repl), static_cast<unsigned long long>(built[0]),
        static_cast<unsigned long long>(built[1]), static_cast<unsigned long long>(built[2]),
        static_cast<unsigned long long>(built[3]), static_cast<unsigned long long>(reused[0]),
        static_cast<unsigned long long>(reused[1]), static_cast<unsigned long long>(reused[2]),
        static_cast<unsigned long long>(reused[3]));
    out += line;
    // sotc: SOTC_TINY_GPL counters, same window. Only printed when a tiny draw was served.
    static u64 p_tgl{}, p_tgq{}, p_tglib{}, p_tglink{}, p_tgsync{}, p_tgsync_ns{};
    const u64 tgl = take(g_tiny_gpl.linked, p_tgl);
    const u64 tgq = take(g_tiny_gpl.queued, p_tgq);
    const u64 tglib = take(g_tiny_gpl.lib_ns, p_tglib);
    const u64 tglink = take(g_tiny_gpl.link_ns, p_tglink);
    // sotc: tiny draws built synchronously on the command processor (any mode; g_tiny_stats).
    const u64 tgsync = take(g_tiny_stats.sync_builds, p_tgsync);
    const u64 tgsync_ns = take(g_tiny_stats.sync_ns, p_tgsync_ns);
    if (tgl != 0 || tgq != 0 || tgsync != 0) {
        char tline[320];
        std::snprintf(tline, sizeof(tline),
                      "SOTC tiny gpl: linked %llu (libraries %.1f ms, links %.1f ms, cp time), "
                      "optimized queued %llu, built sync %llu (%.1f ms, cp time)\n",
                      static_cast<unsigned long long>(tgl), static_cast<double>(tglib) / 1e6,
                      static_cast<double>(tglink) / 1e6, static_cast<unsigned long long>(tgq),
                      static_cast<unsigned long long>(tgsync),
                      static_cast<double>(tgsync_ns) / 1e6);
        out += tline;
    }
    (void)seconds; // the counts are per window; a rate is the reader's division by the window
}

// sotc: N5 counters, per 5 s window (SOTC_N5_PSO=1): the boot prebuild's progress, and the compiles on
// the command processor or the draw thread after the grace period. Call from one thread only.
void ReportN5Pso(std::string& out, double seconds) {
    if (!N5PsoEnabled()) {
        return;
    }
    static bool done_printed{};
    const u64 total = g_n5.boot_total.load(std::memory_order_relaxed);
    if (total != 0 && !done_printed) {
        const u64 built = g_n5.boot_built.load(std::memory_order_relaxed);
        const s64 end = g_n5.boot_end_ns.load(std::memory_order_relaxed);
        const s64 start = g_n5.boot_start_ns.load(std::memory_order_relaxed);
        const s64 at = end ? end : N5NowNs();
        char boot[200];
        std::snprintf(boot, sizeof(boot),
                      "SOTC N5 PSO boot: %llu of %llu graphics pipelines built, %.0f ms%s\n",
                      static_cast<unsigned long long>(built),
                      static_cast<unsigned long long>(total),
                      static_cast<double>(at - start) / 1e6, end ? " (done)" : "");
        out += boot;
        done_printed = end != 0; // one line after the boot is done, then none
    }
    static std::array<u64, kN5Kinds> prev_kind{};
    static u64 prev_grace{};
    u64 window[kN5Kinds]{};
    u64 window_total = 0;
    for (size_t i = 0; i < kN5Kinds; ++i) {
        const u64 now = g_n5.kind[i].load(std::memory_order_relaxed);
        window[i] = now - prev_kind[i];
        prev_kind[i] = now;
        window_total += window[i];
    }
    const u64 grace = g_n5.grace.load(std::memory_order_relaxed);
    const u64 window_grace = grace - prev_grace;
    prev_grace = grace;
    const u64 max_ns = g_n5.max_ns.exchange(0, std::memory_order_relaxed);
    const u64 sum_ns = g_n5.ns.load(std::memory_order_relaxed);
    static u64 prev_sum_ns{};
    const u64 window_ns = sum_ns - prev_sum_ns;
    prev_sum_ns = sum_ns;
    if (window_total == 0 && window_grace == 0) {
        return;
    }
    char line[400];
    std::snprintf(
        line, sizeof(line),
        "SOTC N5 PSO sync compiles after %.0f s: graphics %llu, compute %llu, link %llu, "
        "shader %llu (%.0f/s, %.1f ms, max %.1f ms); during warm-up or the grace period %llu\n",
        N5GraceSeconds(), static_cast<unsigned long long>(window[0]),
        static_cast<unsigned long long>(window[1]), static_cast<unsigned long long>(window[2]),
        static_cast<unsigned long long>(window[3]),
        seconds > 0.0 ? static_cast<double>(window_total) / seconds : 0.0,
        static_cast<double>(window_ns) / 1e6, static_cast<double>(max_ns) / 1e6,
        static_cast<unsigned long long>(window_grace));
    out += line;
}


// sotc: SOTC_PIPE_MANIFEST (gpu/sotc/pipe_manifest.h). The steps below run on the command processor, at the start of
// a full lookup (never in the middle of one: they install programs and permutations).
namespace {
// The permutation of program whose specialization equals the draw's (spec), as GetProgram finds it.
std::optional<u32> ManifestFindPerm(const Program& program, Shader::StageSpecialization& spec) {
    spec.info = &program.info;
    for (size_t p = 0; p < program.modules.size(); ++p) {
        // sotc: SOTC_COMPUTE_RELEASE: a released compute permutation is found too (its pipeline exists).
        if (program.modules[p].Present() && program.modules[p].spec == spec) {
            return static_cast<u32>(p);
        }
    }
    return std::nullopt;
}

bool ManifestStageSupported(const Shader::RuntimeInfo& rt) {
    return (rt.hw_stage == Shader::HwStage::Fragment && rt.sw_stage == Shader::SwStage::Fragment) ||
           (rt.hw_stage == Shader::HwStage::Vertex && rt.sw_stage == Shader::SwStage::Vertex) ||
           (rt.hw_stage == Shader::HwStage::Compute && rt.sw_stage == Shader::SwStage::Compute);
}

std::vector<u8> ManifestSpecBytes(const Shader::StageSpecialization& spec) {
    Serialization::Archive ar;
    spec.Serialize(ar);
    return ar.TakeOff();
}

// Words of one fetch-attribute table segment (dword offsets are u8; a V# is 4 dwords).
constexpr size_t kManifestUdSegment = 260;
} // namespace

SotcPipeManifest::Header PipelineCache::ManifestHeader() const {
    SotcPipeManifest::Header h;
    h.game = std::string{Common::ElfInfo::Instance().GameSerial()};
    h.variant = cache_variant_key;
    h.key_size = static_cast<u32>(sizeof(GraphicsPipelineKey));
    h.rt_size = static_cast<u32>(sizeof(Shader::RuntimeInfo));
    h.meta_version = SotcShaderMetaVersion();
    return h;
}

void PipelineCache::LoadManifest() {
    using Clock = std::chrono::steady_clock;
    manifest = std::make_unique<ManifestState>();
    auto& m = *manifest;
    m.header = ManifestHeader();
    m.loaded = m.last_pump = Clock::now();
    if (!Common::ElfInfo::Instance().IsSotc() || async_workers.empty() || !Storage::DataBase::Instance().IsOpened()) {
        std::fprintf(stderr, "SOTC PIPE MANIFEST: off (%s)\n",
                     !Common::ElfInfo::Instance().IsSotc() ? "not the game's eboot"
                     : async_workers.empty()               ? "no pipeline workers"
                                                           : "no shader cache");
        m.hold_done = true;
        g_manifest_stats.boot_done.store(true, std::memory_order_relaxed);
        return;
    }
    std::string log;
    auto data = SotcPipeManifest::Load(m.header, log);
    std::fputs(log.c_str(), stderr);
    SotcPipeManifest::LoadSkips(m.header.variant);
    std::unordered_map<u64, u32> index_of;
    u32 unreadable = 0;
    const bool throws = Common::g_assert_throws;
    Common::g_assert_throws = true;
    for (auto& rec : data.stages) {
        ManifestState::StageRt st;
        try {
            Serialization::Archive ar{std::vector<u8>(rec.spec)};
            if (!st.spec.Deserialize(ar)) {
                ++unreadable;
                continue;
            }
        } catch (const Serialization::CorruptedData&) {
            ++unreadable;
            continue;
        } catch (const Common::AssertFailure&) {
            ++unreadable;
            continue;
        }
        // sotc: SOTC_STAGE_BINDINGS (on with SOTC_GPL_NODROP): every stage numbers its descriptors from 0 in its own
        // set, so a draw's start binding is 0 where a record made without the switch holds the count of the stages
        // bound before (the pixel stage's resources, for a vertex stage). That count is the only part of a
        // specialization the switch changes (the buffer slot count is shared by the stages either way): the record
        // is read as the draw makes it now. Its id stays the recorded one; a capture of the same stage under the
        // switch has another id and is appended once.
        if (profile.stage_bindings && st.spec.start.unified != 0) {
            st.spec.start.unified = 0;
            ++m.start_rebased;
        }
        st.hint = std::make_shared<const SotcPretranslate::Hint>(rec.hint);
        st.cache_hash = rec.variant == Shader::StageVariantDefault ? rec.hash : HashCombine(rec.hash, u64{rec.variant});
        st.late = SotcPipeManifest::Skipped(rec.id, SotcPipeManifest::SkipKind::Late);
        st.rec = std::move(rec);
        index_of.emplace(st.rec.id, static_cast<u32>(m.stages.size()));
        m.stages.push_back(std::move(st));
    }
    // sotc: SOTC_MANIFEST_HOLD_FAST: every stage starts Pending (StageRt's default); SetState keeps the counts.
    m.pending = static_cast<u32>(
        std::ranges::count(m.stages, ManifestState::St::Pending, &ManifestState::StageRt::state));
    m.waiting = 0;
    Common::g_assert_throws = throws;
    u32 graphics = 0, compute = 0;
    for (const auto& rec : data.pipes) {
        ManifestState::PipeRt p;
        p.compute = rec.compute;
        bool ok = true;
        for (const auto& [slot, id] : rec.stages) {
            const auto it = index_of.find(id);
            if (it == index_of.end() || slot >= MaxShaderStages) {
                ok = false;
                break;
            }
            p.stages.emplace_back(slot, it->second);
        }
        if (!ok) {
            continue;
        }
        if (!p.compute) {
            std::memcpy(&p.key, rec.key.data(), sizeof(p.key));
            p.key.stage_hashes = {};
        }
        (p.compute ? compute : graphics)++;
        m.pipes.push_back(std::move(p));
    }
    // sotc: SOTC_GPL_PREDICT: the manifest's VS+PS pipelines give the most common pre-rasterization and fragment
    // states before any draw (the predicted library jobs of a pass whose draws gave no key yet).
    if (SotcNonBlocking::GplPredict()) {
        if (!lib_predict) {
            lib_predict = std::make_shared<LibPredictState>();
        }
        for (const auto& p : m.pipes) {
            const bool vs_ps = !p.compute && p.stages.size() == 2 &&
                               std::ranges::all_of(p.stages, [](const auto& st) {
                                   return st.first == u32(Shader::SwStage::Vertex) ||
                                          st.first == u32(Shader::SwStage::Fragment);
                               });
            if (vs_ps) {
                lib_predict->Note(p.key);
            }
        }
    }
    std::vector<u64> wanted;
    wanted.reserve(m.stages.size());
    for (const auto& st : m.stages) {
        wanted.push_back(st.rec.hash);
    }
    SotcPipeManifest::SetWanted(wanted);
    std::fprintf(stderr,
                 "SOTC PIPE MANIFEST: %zu pipelines (%u graphics, %u compute), %zu stages; shipped %zu, user %zu; "
                 "unreadable stages %u%s; start bindings rebased for the stage sets %u\n",
                 m.pipes.size(), graphics, compute, m.stages.size(), data.shipped_pipes, data.user_pipes, unreadable,
                 data.variant_differs ? "; recorded under another shader cache variant (accepted)" : "",
                 m.start_rebased);
    if (m.pipes.empty()) {
        m.hold_done = true;
        g_manifest_stats.boot_done.store(true, std::memory_order_relaxed);
        SotcPipeManifest::SetWanted({});
        SotcPipeManifest::DropCode();
    }
}

void PipelineCache::ManifestUserData(AsyncPipelineJob::Translate& t) {
    if (!t.manifest_fetch || !t.manifest_hint) {
        return;
    }
    // One table segment per SGPR pair that holds a V# table pointer; each V# at its dword offset, synthetic but for the
    // fields its draw had (as the resource pass builds a buffer's synthetic sharp).
    std::array<s32, Shader::ShaderParams::NumShaderUserData> segment{};
    segment.fill(-1);
    s32 segments = 0;
    for (const auto& attrib : t.manifest_fetch->attributes) {
        if (attrib.sgpr_base + 1u < segment.size() && segment[attrib.sgpr_base] < 0) {
            segment[attrib.sgpr_base] = segments++;
        }
    }
    t.manifest_ud_table.assign(static_cast<size_t>(segments) * kManifestUdSegment, 0u);
    for (const auto& h : t.manifest_hint->attribs) {
        const u32 sgpr = static_cast<u32>(h.key >> 8), dword = static_cast<u32>(h.key & 0xFF);
        if (sgpr >= segment.size() || segment[sgpr] < 0) {
            continue;
        }
        AmdGpu::Buffer vsharp{};
        vsharp.base_address = 0x100000;
        vsharp.num_records = 0x10000;
        vsharp.stride = h.stride;
        vsharp.swizzle_enable = h.swizzle;
        vsharp.index_stride = h.index_stride;
        vsharp.element_size = h.element_size;
        vsharp.num_format = h.num_format;
        vsharp.data_format = h.data_format;
        vsharp.dst_sel_x = h.dst_sel[0];
        vsharp.dst_sel_y = h.dst_sel[1];
        vsharp.dst_sel_z = h.dst_sel[2];
        vsharp.dst_sel_w = h.dst_sel[3];
        std::memcpy(t.manifest_ud_table.data() + static_cast<size_t>(segment[sgpr]) * kManifestUdSegment + dword,
                    &vsharp, sizeof(vsharp));
    }
    for (u32 sgpr = 0; sgpr < segment.size(); ++sgpr) {
        if (segment[sgpr] < 0) {
            continue;
        }
        const u64 address = reinterpret_cast<uintptr_t>(t.manifest_ud_table.data() +
                                                        static_cast<size_t>(segment[sgpr]) * kManifestUdSegment);
        t.user_data[sgpr] = static_cast<u32>(address);
        t.user_data[sgpr + 1] = static_cast<u32>(address >> 32);
    }
}

void PipelineCache::ManifestQueueTranslate(u32 index, const std::vector<u32>& code, bool holding) {
    auto& m = *manifest;
    auto& st = m.stages[index];
    auto job = std::make_unique<AsyncPipelineJob>();
    auto& t = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
    t.hw_stage = st.spec.runtime_info.hw_stage;
    t.sw_stage = st.spec.runtime_info.sw_stage;
    t.hash = st.cache_hash;
    t.variant = st.rec.variant;
    t.guest_hash = st.rec.variant == Shader::StageVariantDefault ? 0 : st.rec.hash;
    t.owned_code = code;
    t.code = t.owned_code;
    t.runtime_info = st.spec.runtime_info;
    t.start = st.spec.start;
    t.pretranslate = true; // synthetic sharps: there is no draw
    t.manifest_stage = static_cast<s32>(index);
    t.manifest_hint = st.hint;
    if (!st.spec.fetch_shader_data.Empty()) {
        t.manifest_fetch = st.spec.fetch_shader_data;
    }
    m.SetState(st, ManifestState::St::Queued);
    ++m.translating;
    {
        std::scoped_lock lock{async_mutex};
        PushJobLocked(holding ? JobLane::Translate : JobLane::Background, std::move(job));
    }
    async_cv.notify_one();
}

bool PipelineCache::ManifestResolveStage(u32 index, bool may_translate) {
    auto& m = *manifest;
    auto& st = m.stages[index];
    const auto fail = [&](const char* reason, bool remember) {
        m.SetState(st, ManifestState::St::Failed);
        st.reason = reason;
        if (remember) {
            SotcPipeManifest::NoteSkip(st.rec.id, SotcPipeManifest::SkipKind::Failed, reason);
        }
        m.dirty = true;
        return true;
    };
    if (pending_programs.contains(st.cache_hash)) {
        return false; // a draw's translation of the program is on a worker: looked at again later
    }
    auto found = program_cache.find(st.cache_hash);
    if (found == program_cache.end() && lazy_programs.contains(st.cache_hash)) {
        LoadLazyProgram(st.cache_hash); // its permutations from the disk
        last_graphics_pipeline = nullptr;
        pipe_fast_gfx.valid = false;
        pipe_fast_cs.valid = false;
        found = program_cache.find(st.cache_hash);
    }
    if (found != program_cache.end()) {
        Program& program = *found.value();
        if (const auto perm = ManifestFindPerm(program, st.spec)) {
            m.SetState(st, ManifestState::St::Ready);
            st.program = &program;
            st.perm = *perm;
            ++m.stage_cached;
            m.dirty = true;
            return true;
        }
        if (std::ranges::find(program.pending_specs, st.spec) != program.pending_specs.end()) {
            return false; // a draw's permutation of it is on a worker
        }
    }
    if (SotcPipeManifest::Skipped(st.rec.id, SotcPipeManifest::SkipKind::Failed)) {
        return fail("failed before", false);
    }
    if (!ManifestStageSupported(st.spec.runtime_info)) {
        return fail("stage kind", true);
    }
    // A vertex stage with a fetch shader: every attribute's V# must have come with the record.
    for (const auto& attrib : st.spec.fetch_shader_data.attributes) {
        const u64 key = (u64{attrib.sgpr_base} << 8) | attrib.dword_offset;
        if (attrib.sgpr_base + 1u >= Shader::ShaderParams::NumShaderUserData ||
            std::ranges::find(st.hint->attribs, key, &SotcPretranslate::HintBuffer::key) == st.hint->attribs.end()) {
            return fail("fetch attributes", true);
        }
    }
    const auto code = SotcPipeManifest::Code(st.rec.hash);
    if (!code) {
        if (st.state != ManifestState::St::Waiting) {
            m.SetState(st, ManifestState::St::Waiting);
        }
        return false;
    }
    if (!may_translate) {
        m.SetState(st, ManifestState::St::Pending); // its code is here: translated when a slot frees
        return false;
    }
    ManifestQueueTranslate(index, *code, m.boot_lanes);
    return true;
}

void PipelineCache::ManifestInstallStage(AsyncPipelineJob::Translate& t) {
    auto& m = *manifest;
    if (m.translating > 0) {
        --m.translating;
    }
    auto& st = m.stages[static_cast<u32>(t.manifest_stage)];
    m.dirty = true;
    const auto destroy = [&] {
        if (t.module) {
            instance.GetDevice().destroyShaderModule(t.module);
            t.module = vk::ShaderModule{};
        }
    };
    const auto fail = [&](const char* reason) {
        destroy();
        m.SetState(st, ManifestState::St::Failed);
        st.reason = reason;
        SotcPipeManifest::NoteSkip(st.rec.id, SotcPipeManifest::SkipKind::Failed, reason);
    };
    if (t.failed || !t.new_program || !t.spec || !t.module) {
        fail("translation failed");
        return;
    }
    // The check that makes the stage the draw's: the translation's specialization (made from the synthetic sharps with
    // the draw's fields) equals the recorded one, both ways (the operator walks the left side's lists).
    st.spec.info = &t.new_program->info;
    if (!(*t.spec == st.spec) || !(st.spec == *t.spec)) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
            std::fprintf(stderr,
                         "SOTC PIPE MANIFEST: stage %016llx (program %#llx, sw stage %u) not rebuilt: its translation's "
                         "specialization differs from the draw's (buffers %zu/%zu, images %zu/%zu, samplers %zu/%zu, "
                         "fetch %d, diff mask %08x)\n",
                         static_cast<unsigned long long>(st.rec.id), static_cast<unsigned long long>(st.rec.hash),
                         static_cast<u32>(t.sw_stage), t.spec->buffers.size(), st.spec.buffers.size(),
                         t.spec->images.size(), st.spec.images.size(), t.spec->samplers.size(),
                         st.spec.samplers.size(), int{t.spec->fetch_shader_data == st.spec.fetch_shader_data},
                         PretranslateDiff(*t.spec, st.spec)); // sotc: DiffBit in pretranslate.h, never accept a mismatch
        }
        fail("specialization differs");
        return;
    }
    // The program's permutations on disk first: loaded later, they would take their indices over ours.
    if (lazy_programs.contains(st.cache_hash)) {
        LoadLazyProgram(st.cache_hash);
    }
    auto [it, is_new] = program_cache.try_emplace(st.cache_hash);
    Program* program = nullptr;
    u32 perm = 0;
    if (is_new) {
        it.value() = std::move(t.new_program);
        program = it.value().get();
        RegisterShaderBinary(std::move(t.spv), st.cache_hash, 0);
        RegisterShaderMeta(program->info, t.spec->fetch_shader_data, *t.spec, HashCombine(st.cache_hash, u64{0}), 0);
        t.spec->info = &program->info;
        program->AddPermut(t.module, std::move(*t.spec));
        program->info.UserData() = {}; // pointed into the job (its draws set their own)
    } else {
        program = it.value().get();
        if (const auto found = ManifestFindPerm(*program, st.spec)) {
            destroy(); // a draw (or another record of the same key) made it meanwhile
            perm = *found;
        } else if (!SameBindingLayout(program->info, t.new_program->info)) {
            fail("resources differ from the program's");
            return;
        } else {
            perm = static_cast<u32>(program->modules.size());
            Shader::CombineBindlessCache(program->info.bindless, t.new_program->info.bindless);
            RegisterShaderBinary(std::move(t.spv), st.cache_hash, perm);
            RegisterShaderMeta(program->info, t.spec->fetch_shader_data, *t.spec,
                               HashCombine(st.cache_hash, u64{perm}), perm);
            t.spec->info = &program->info;
            program->AddPermut(t.module, std::move(*t.spec));
        }
    }
    t.module = vk::ShaderModule{};
    m.SetState(st, ManifestState::St::Ready);
    st.program = program;
    st.perm = perm;
    ++m.stage_translated;
    // The program's permutations moved: no record may keep a pointer into them.
    last_graphics_pipeline = nullptr;
    pipe_fast_gfx.valid = false;
    pipe_fast_cs.valid = false;
}

void PipelineCache::ManifestSchedulePipes(bool holding) {
    auto& m = *manifest;
    for (auto& p : m.pipes) {
        if (p.state != ManifestState::Ps::Waiting) {
            continue;
        }
        bool ready = true;
        const char* failed = nullptr;
        for (const auto& [slot, si] : p.stages) {
            auto& st = m.stages[si];
            if (st.state == ManifestState::St::Failed) {
                failed = st.reason ? st.reason : "stage failed";
                break;
            }
            // The program is looked up again: a program may have left the cache since the stage was resolved (the
            // eviction at the end of the SOTC_PRETRANSLATE hold); the stage is then resolved again.
            if (st.state == ManifestState::St::Ready) {
                const auto found = program_cache.find(st.cache_hash);
                // sotc: SOTC_COMPUTE_RELEASE: a released compute permutation stays ready while its pipeline exists.
                const bool present =
                    found != program_cache.end() && st.perm < found.value()->modules.size() &&
                    (found.value()->modules[st.perm].module ||
                     (found.value()->modules[st.perm].sotc_released &&
                      compute_pipelines.contains(
                          ComputePipelineKey{HashCombine(found.value()->info.CacheHash(), u64{st.perm})})));
                if (!present) {
                    m.SetState(st, ManifestState::St::Pending);
                    st.program = nullptr;
                } else {
                    st.program = found.value().get();
                }
            }
            ready = ready && st.state == ManifestState::St::Ready;
        }
        if (failed) {
            p.state = ManifestState::Ps::Skipped;
            ++m.skipped[failed];
            continue;
        }
        if (!ready) {
            continue;
        }
        if (p.compute) {
            const auto& st = m.stages[p.stages[0].second];
            Program& program = *st.program;
            const ComputePipelineKey key{HashCombine(program.info.CacheHash(), u64{st.perm})};
            if (compute_pipelines.contains(key)) {
                p.state = ManifestState::Ps::Done;
                ++m.from_disk;
                continue;
            }
            ComputePipeline::SerializationSupport sdata{};
            auto pipe = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, layout_cache,
                                                          StageProfile(program.info.variant), *pipeline_cache, key,
                                                          program.info, program.modules[st.perm].module, sdata,
                                                          false, true);
            ComputePipeline* raw = pipe.get();
            compute_pipelines.try_emplace(key).first.value() = std::move(pipe);
            last_compute_pipeline = nullptr;
            RegisterPipelineData(key, sdata);
            ++num_new_pipelines;
            if (SotcNonBlocking::ComputeRelease()) { // sotc: its module leaves once built (ReleaseBuiltComputeModules)
                compute_release_queue.push_back({st.cache_hash, st.perm, key.value});
            }
            auto job = std::make_unique<AsyncPipelineJob>();
            job->compute = raw;
            job->manifest = true;
            ++m.compute_queued;
            if (!holding) {
                g_manifest_stats.late_compute.fetch_add(1, std::memory_order_relaxed);
            }
            {
                std::scoped_lock lock{async_mutex};
                PushJobLocked(holding ? JobLane::Compute : JobLane::Background, std::move(job));
            }
            async_cv.notify_one();
            p.state = ManifestState::Ps::Queued;
            continue;
        }
        GraphicsPipelineKey key = p.key;
        std::array<const Shader::Info*, MaxShaderStages> stage_infos{};
        std::array<vk::ShaderModule, MaxShaderStages> stage_modules{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> stage_runtime = runtime_infos;
        std::optional<Shader::Gcn::FetchShaderData> stage_fetch;
        bool dynamic_mip = false;
        for (const auto& [slot, si] : p.stages) {
            const auto& st = m.stages[si];
            Program& program = *st.program;
            const auto& module = program.modules[st.perm];
            key.stage_hashes[slot] = HashCombine(program.info.CacheHash(), u64{st.perm});
            stage_infos[slot] = &program.info;
            stage_modules[slot] = module.module;
            stage_runtime[slot] = module.spec.runtime_info;
            if (!module.spec.fetch_shader_data.Empty()) {
                stage_fetch = module.spec.fetch_shader_data;
            }
            for (const auto& image : program.info.images) {
                dynamic_mip |= image.array_size == 0 &&
                               image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex;
            }
        }
        // sotc: SOTC_GPL_NODROP: the libraries of the pipeline (vertex input, both shader stages, fragment output),
        // for the draws whose key no manifest holds: a key made of known stages and a known output state then only
        // links. Built for pipelines found on disk too (a library is not saved with the pipeline). Without the link
        // (SetLibrariesOnly); the libraries are deduplicated by their state (GplLibraryCache).
        if (SotcNonBlocking::GplNoDrop() && instance.IsGraphicsPipelineLibrary() &&
            !instance.IsDescriptorHeapEnabled() && instance.IsVertexInputDynamicState() &&
            !EmulatorSettings.IsShaderCollect() && !dynamic_mip &&
            GraphicsPipeline::FastLinkable(key, stage_infos, stage_runtime, StagesProfile(stage_infos))) {
            auto job = std::make_unique<AsyncPipelineJob>();
            job->key = key;
            job->hash = std::hash<GraphicsPipelineKey>{}(key);
            job->infos = stage_infos;
            job->runtime_infos = stage_runtime;
            job->fetch_shader = stage_fetch;
            job->modules = stage_modules;
            job->link_only = true;
            job->warm_lib = true;
            job->manifest_libs = true;
            ++m.libs_building;
            ++m.libs_queued;
            g_nodrop.boot_libs_queued.fetch_add(1, std::memory_order_relaxed);
            // sotc: SOTC_GPL_LIB_PHASE: in the first boot window the libraries wait for the hold's library phase.
            if (SotcNonBlocking::GplLibPhase() && !m.held_once && !m.hold_done && !m.lib_phase_done) {
                m.lib_backlog.push_back(std::move(job));
            } else {
                {
                    std::scoped_lock lock{async_mutex};
                    // sotc: SOTC_PLAY_QUIET: a late stage's libraries go to the Predict lane (never the capped one).
                    PushJobLocked(holding                        ? JobLane::Graphics
                                  : SotcNonBlocking::PlayQuiet() ? JobLane::Predict
                                                                 : JobLane::Background,
                                  std::move(job));
                }
                async_cv.notify_one();
            }
        }
        if (graphics_pipelines.contains(key)) {
            p.state = ManifestState::Ps::Done;
            ++m.from_disk;
            continue;
        }
        // Only pipelines whose construction reads no draw state (AsyncIneligibleReason's rules).
        if (!instance.IsVertexInputDynamicState() || EmulatorSettings.IsShaderCollect() || dynamic_mip) {
            p.state = ManifestState::Ps::Skipped;
            ++m.skipped["needs draw state"];
            continue;
        }
        graphics_pipelines.try_emplace(key); // null until collected, as a draw's queued pipeline
        auto job = std::make_unique<AsyncPipelineJob>();
        job->key = key;
        job->hash = std::hash<GraphicsPipelineKey>{}(key);
        job->infos = stage_infos;
        job->runtime_infos = stage_runtime;
        job->fetch_shader = stage_fetch;
        job->modules = stage_modules;
        job->manifest = true;
        ++m.graphics_building;
        if (!holding) {
            g_manifest_stats.late_graphics.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lock{async_mutex};
            // sotc: SOTC_PLAY_QUIET: a late stage's build goes to the Predict lane (ahead of the optimized builds).
            PushJobLocked(holding                        ? JobLane::Graphics
                          : SotcNonBlocking::PlayQuiet() ? JobLane::Predict
                                                         : JobLane::Background,
                          std::move(job));
        }
        async_cv.notify_one();
        p.state = ManifestState::Ps::Queued;
    }
}

void PipelineCache::ManifestPump(bool holding) {
    auto& m = *manifest;
    m.last_pump = std::chrono::steady_clock::now();
    if (!m.finished.empty()) {
        auto finished = std::move(m.finished);
        m.finished.clear();
        for (auto& t : finished) {
            ManifestInstallStage(*t);
        }
    }
    // Before and during a boot hold every worker serves the manifest; after the first hold the stages are translated
    // two at a time on the background lane.
    m.boot_lanes = manifest_holding || (holding && !m.held_once);
    const u32 cap = m.boot_lanes ? ~0u : 2u;
    const u64 arrivals = SotcPipeManifest::CodeArrivals();
    const bool new_code = arrivals != m.seen_arrivals;
    m.seen_arrivals = arrivals;
    // sotc: SOTC_MANIFEST_HOLD_FAST: no pass when no stage is Pending and no Waiting stage can have got its code (the
    // pass would resolve nothing); the Waiting count is kept by SetState.
    if (SotcNonBlocking::ManifestHoldFast()) {
        if (m.pending != 0 || (new_code && m.waiting != 0)) {
            for (u32 i = 0; i < m.stages.size(); ++i) {
                auto& st = m.stages[i];
                if (st.state == ManifestState::St::Pending ||
                    (st.state == ManifestState::St::Waiting && new_code)) {
                    ManifestResolveStage(i, m.translating < cap);
                }
            }
        }
        g_manifest_stats.waiting_code.store(m.waiting, std::memory_order_relaxed);
    } else {
        u32 waiting = 0;
        for (u32 i = 0; i < m.stages.size(); ++i) {
            auto& st = m.stages[i];
            if (st.state == ManifestState::St::Pending ||
                (st.state == ManifestState::St::Waiting && new_code)) {
                ManifestResolveStage(i, m.translating < cap);
            }
            waiting += st.state == ManifestState::St::Waiting;
        }
        g_manifest_stats.waiting_code.store(waiting, std::memory_order_relaxed);
    }
    if (m.dirty) {
        m.dirty = false;
        ManifestSchedulePipes(m.boot_lanes);
    }
}

void PipelineCache::MaybeHoldForManifest(bool compute) {
    using Clock = std::chrono::steady_clock;
    if (!manifest) {
        if (!warmup_done.load(std::memory_order_acquire)) {
            return;
        }
        LoadManifest();
    }
    auto& m = *manifest;
    // sotc: SOTC_MANIFEST_HOLD_FAST: a compute lookup only installs finished translations; the timed pump and the hold
    // decisions run on the graphics lookups (a loading screen draws).
    const bool fast = SotcNonBlocking::ManifestHoldFast();
    if (fast && compute && m.finished.empty()) {
        return;
    }
    const auto now = Clock::now();
    if (SotcPretranslate::Precompile()) {
        return; // the shader preparation run: it only records (and exits by itself)
    }
    if (m.hold_done) {
        if (!m.pipes.empty() && (!m.finished.empty() || now - m.last_pump > std::chrono::milliseconds{100})) {
            ManifestPump(false);
        }
        return;
    }
    // The boot window: from the first shader file the game loads (the game-start loading screen) for 2 minutes. The
    // stages found in the caches are resolved and their pipelines queued from the first lookup on.
    const bool loading = SotcPipeManifest::LastProgramNs() != 0;
    if (loading && m.window_start == Clock::time_point{}) {
        m.window_start = now;
    }
    if (!m.finished.empty() || now - m.last_pump > std::chrono::milliseconds{50}) {
        ManifestPump(true);
    }
    if (now - (loading ? m.window_start : m.loaded) > std::chrono::minutes{2}) {
        // The window is over: the stages whose code did not come in it are not waited for by the next boot holds
        // (they are still made when their code comes, after the boot).
        u32 late = 0;
        for (auto& st : m.stages) {
            if (st.state == ManifestState::St::Waiting) {
                ++late;
                if (!st.late) {
                    st.late = true;
                    SotcPipeManifest::NoteSkip(st.rec.id, SotcPipeManifest::SkipKind::Late, "code not loaded at boot");
                }
            }
        }
        m.hold_done = true;
        g_manifest_stats.boot_done.store(true, std::memory_order_relaxed);
        ManifestFlushLibraries(JobLane::Background); // sotc: SOTC_GPL_LIB_PHASE, none held back from now on
        std::fprintf(stderr, "SOTC PIPE MANIFEST: boot window over (%s); stages waiting for their code %u\n",
                     loading ? "2 minutes after the first shader file" : "no shader file loaded in 2 minutes", late);
        if (late == 0) {
            SotcPipeManifest::SetWanted({});
            SotcPipeManifest::DropCode();
        }
        return;
    }
    if (!loading) {
        return;
    }
    bool work = false;
    if (!m.held_once) {
        // The first hold: as soon as the manifest has work (translations, builds) and the game loads shader files.
        work = m.translating != 0 || m.graphics_building != 0 || !m.finished.empty() || m.libs_building != 0 ||
               g_manifest_stats.compute_built.load(std::memory_order_relaxed) < m.compute_queued;
        if (fast) {
            work = work || m.pending != 0; // sotc: SOTC_MANIFEST_HOLD_FAST (kept by SetState)
        }
        for (size_t i = 0; i < m.stages.size() && !work && !fast; ++i) {
            work = m.stages[i].state == ManifestState::St::Pending;
        }
    } else if (fast) {
        // sotc: SOTC_MANIFEST_HOLD_FAST: the same decision as below from the kept Pending count.
        if (m.pending >= 32) {
            if (SotcNonBlocking::PlayQuiet() && !BbOverlay::LoadingShown()) {
                g_manifest_stats.quiet_deferred.fetch_add(1, std::memory_order_relaxed);
            } else {
                work = true;
            }
        }
    } else {
        // After it, only a burst of code (another loading screen in the window: 32 stages or more ready to translate);
        // a trickle is translated in the background, so play is never held.
        u32 ready = 0;
        for (const auto& st : m.stages) {
            ready += st.state == ManifestState::St::Pending;
        }
        work = ready >= 32;
        // sotc: SOTC_PLAY_QUIET: never a hold while the game draws; the stages go to the Predict lane instead
        // (ManifestSchedulePipes with holding false).
        if (work && SotcNonBlocking::PlayQuiet() && !BbOverlay::LoadingShown()) {
            work = false;
            g_manifest_stats.quiet_deferred.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (work) {
        RunManifestHold();
    }
}

void PipelineCache::ManifestFlushLibraries(JobLane lane) {
    auto& m = *manifest;
    m.lib_phase_done = true;
    if (m.lib_backlog.empty()) {
        return;
    }
    {
        std::scoped_lock lock{async_mutex};
        for (auto& job : m.lib_backlog) {
            PushJobLocked(lane, std::move(job));
        }
    }
    m.lib_backlog.clear();
    async_cv.notify_all();
}

void PipelineCache::RunManifestHold() {
    using Clock = std::chrono::steady_clock;
    auto& m = *manifest;
    // Every worker translates and builds the manifest's pipelines while the loading screen stays up.
    const auto start = Clock::now();
    const uint64_t private_start = SotcPipeManifest::ProcessPrivateBytes(); // sotc: SOTC_GPL_NODROP line below
    manifest_holding = true;
    SetCompileHold(true, "manifest hold start"); // sotc: SOTC_COMPILE_THREADS, SOTC RAM
    if (m.hold_start == Clock::time_point{}) {
        m.hold_start = start;
    }
    last_graphics_pipeline = nullptr;
    pipe_fast_gfx.valid = false;
    pipe_fast_cs.valid = false;
    SotcStats::prep_start_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch()).count(),
        std::memory_order_relaxed);
    SotcStats::prep_total.store(static_cast<u32>(m.pipes.size()), std::memory_order_relaxed);
    SotcStats::prep_done.store(0, std::memory_order_relaxed);
    SotcStats::prep_active.store(true, std::memory_order_release);
    std::fprintf(stderr, "SOTC PIPE MANIFEST: holding the loading screen for %zu pipelines (%zu stages)\n",
                 m.pipes.size(), m.stages.size());
    m.last_progress = start;
    for (;;) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            CollectAsyncPipelines();
        }
        ManifestPump(true);
        u32 done = 0, waiting_pipes = 0;
        for (const auto& p : m.pipes) {
            if (p.state == ManifestState::Ps::Done || p.state == ManifestState::Ps::Skipped) {
                ++done;
            } else if (p.state == ManifestState::Ps::Waiting) {
                // Waiting for a stage. A stage whose code did not come in an earlier boot window is not waited for.
                bool waits = false;
                for (const auto& [slot, si] : p.stages) {
                    const auto& st = m.stages[si];
                    waits |= (st.state == ManifestState::St::Waiting && !st.late) ||
                             st.state == ManifestState::St::Pending || st.state == ManifestState::St::Queued;
                }
                waiting_pipes += waits;
            }
        }
        const u64 compute_built = g_manifest_stats.compute_built.load(std::memory_order_relaxed);
        const u64 built = m.built_graphics + m.refused + compute_built;
        SotcStats::prep_done.store(static_cast<u32>(std::min<u64>(done + built, m.pipes.size())),
                                   std::memory_order_relaxed);
        const u32 writes = Storage::DataBase::Instance().PendingWrites();
        SotcStats::prep_writes.store(writes, std::memory_order_relaxed);
        // sotc: SOTC_GPL_PREDICT (shader-only keys): the libraries of the materials the game created so far are built
        // in the hold too (every waiting slot each pass), and the hold waits for their jobs.
        if (SotcNonBlocking::GplPredict()) {
            PredictLibraries(nullptr);
        }
        const bool predict_busy = SotcNonBlocking::GplPredict() &&
                                  lib_jobs_pending.load(std::memory_order_relaxed) != 0;
        const bool busy = m.translating != 0 || m.graphics_building != 0 || compute_built < m.compute_queued ||
                          !m.finished.empty() || m.libs_building != 0 || // sotc: SOTC_GPL_NODROP libraries
                          predict_busy;
        const auto t = Clock::now();
        const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        // The game stopped loading shader files for 3 s: the code that has not come belongs to later areas.
        const bool load_idle = now_ns - SotcPipeManifest::LastProgramNs() > 3'000'000'000LL;
        // sotc: SOTC_GPL_LIB_PHASE: the translations and pipelines are done (as the end condition below sees them):
        // the private memory is sampled, then the held-back libraries are queued (the hold waits for them as before).
        if (!m.lib_phase_done && !m.lib_backlog.empty() && m.translating == 0 && m.graphics_building == 0 &&
            compute_built >= m.compute_queued && m.finished.empty() && (waiting_pipes == 0 || load_idle)) {
            m.private_mid = SotcPipeManifest::ProcessPrivateBytes();
            ManifestFlushLibraries(JobLane::Graphics);
            continue;
        }
        if ((!busy && (waiting_pipes == 0 || load_idle) && writes < 64) || t - start > std::chrono::minutes{45}) {
            break;
        }
        if (t - m.last_progress > std::chrono::seconds{2}) {
            m.last_progress = t;
            std::fprintf(stderr,
                         "SOTC PIPE MANIFEST: holding %.1f s: stages translated %u, from the caches %u, translating "
                         "%u; pipelines built %llu of %zu (queued graphics %u, compute %llu), waiting for code %u\n",
                         std::chrono::duration<double>(t - start).count(), m.stage_translated, m.stage_cached,
                         m.translating, static_cast<unsigned long long>(m.built_graphics + compute_built),
                         m.pipes.size(), m.graphics_building,
                         static_cast<unsigned long long>(m.compute_queued - std::min(m.compute_queued, compute_built)),
                         g_manifest_stats.waiting_code.load(std::memory_order_relaxed));
        }
        std::unique_lock lock{async_mutex};
        translate_done_cv.wait_for(lock, std::chrono::milliseconds{20},
                                   [this] { return async_done_count.load(std::memory_order_relaxed) != 0; });
    }
    ManifestFlushLibraries(JobLane::Background); // sotc: SOTC_GPL_LIB_PHASE (the hold timed out with libraries held)
    u32 not_done = 0;
    for (const auto& p : m.pipes) {
        not_done += p.state == ManifestState::Ps::Waiting;
    }
    const u64 compute_built = g_manifest_stats.compute_built.load(std::memory_order_relaxed);
    const bool built_new = m.built_graphics + compute_built != m.saved_built;
    m.saved_built = m.built_graphics + compute_built;
    // Written before play, as the pretranslate hold does: the shader index and the driver's pipelines.
    SaveShaderIndex();
    if (built_new) {
        if (SotcNonBlocking::Io() || SotcNonBlocking::PlayQuiet()) {
            RequestDriverCacheSave(); // sotc: SOTC_PLAY_QUIET: never on the command processor
        } else {
            SaveDriverCache();
        }
    }
    SotcPipeManifest::Flush();
    manifest_holding = false;
    SetCompileHold(false, "manifest hold end"); // sotc: SOTC_COMPILE_THREADS, SOTC RAM
    ReleaseBuiltComputeModules(true);           // sotc: SOTC_COMPUTE_RELEASE: the manifest's compute modules
    m.held_once = true;
    const auto end = Clock::now();
    m.held += end - start;
    SotcStats::prep_active.store(false, std::memory_order_release);
    std::string reasons;
    u32 skipped = 0;
    for (const auto& [reason, count] : m.skipped) {
        skipped += count;
        reasons += (reasons.empty() ? "" : ", ") + reason + " " + std::to_string(count);
    }
    if (not_done != 0) {
        reasons += (reasons.empty() ? "" : ", ") + std::string{"code not loaded yet (built when it comes) "} +
                   std::to_string(not_done);
    }
    std::fprintf(stderr,
                 "SOTC PIPE MANIFEST: built %llu graphics + %llu compute in %lld ms (from disk %llu), held loading "
                 "%lld ms; skipped %u (%s)\n",
                 static_cast<unsigned long long>(m.built_graphics), static_cast<unsigned long long>(compute_built),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(end - m.loaded).count()),
                 static_cast<unsigned long long>(m.from_disk),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(m.held).count()),
                 skipped + not_done, reasons.empty() ? "none" : reasons.c_str());
    // sotc: SOTC_GPL_NODROP: the libraries the hold built (all kept for the session), and the private memory of the
    // process across the hold (pipelines, translations and libraries together: an upper bound for the libraries).
    if (SotcNonBlocking::GplNoDrop()) {
        using L = GplLibraryCache;
        const size_t vi = gpl_libs.Count(L::VertexInput), pr = gpl_libs.Count(L::PreRaster),
                     fs = gpl_libs.Count(L::Fragment), fo = gpl_libs.Count(L::FragmentOutput);
        const uint64_t private_end = SotcPipeManifest::ProcessPrivateBytes();
        // sotc: SOTC_GPL_LIB_PHASE: the growth split at the start of the library phase (sampled in this hold only).
        const auto mb = [](uint64_t a, uint64_t b) {
            return (static_cast<double>(a) - static_cast<double>(b)) / 1048576.0;
        };
        const bool split = m.private_mid != 0 && m.private_mid >= private_start;
        std::fprintf(stderr,
                     "SOTC GPL NODROP: boot libraries for %llu manifest pipelines; libraries held %zu (vertex input %zu, "
                     "pre-rasterization %zu, fragment %zu, fragment output %zu); process private memory %.0f MB "
                     "(%+.0f MB across the hold%s%.0f MB%s%.0f MB%s)\n",
                     static_cast<unsigned long long>(m.libs_queued), vi + pr + fs + fo, vi, pr, fs, fo,
                     static_cast<double>(private_end) / 1048576.0, mb(private_end, private_start),
                     split ? ": translations + pipelines " : "", split ? mb(m.private_mid, private_start) : 0.0,
                     split ? ", libraries " : "", split ? mb(private_end, m.private_mid) : 0.0,
                     split ? "" : "; phases not split");
        m.private_mid = 0;
    }
}


bool PipelineCache::CaptureManifestGraphics() {
    SotcPipeManifest::Pipe pipe;
    pipe.compute = false;
    GraphicsPipelineKey key = graphics_key;
    key.stage_hashes = {};
    pipe.key.resize(sizeof(key));
    std::memcpy(pipe.key.data(), &key, sizeof(key));
    const auto& header = manifest->header;
    std::vector<SotcPipeManifest::Stage> stages;
    for (u32 slot = 0; slot < MaxShaderStages; ++slot) {
        const Shader::Info* info = infos[slot];
        if (!info) {
            continue;
        }
        // Only the stage kinds a manifest translation can make (the others need draw memory: tessellation constants,
        // geometry copy shaders).
        const auto found = program_cache.find(info->CacheHash());
        const auto perm = PermOfStage(graphics_key.stage_hashes[slot], info->CacheHash());
        if (found == program_cache.end() || !perm || *perm >= found.value()->modules.size()) {
            return true;
        }
        const auto& spec = found.value()->modules[*perm].spec;
        if (!ManifestStageSupported(spec.runtime_info)) {
            return true;
        }
        // sotc: a module can match a draw with null bindings. Those hints cannot reproduce
        // its stored specialization: retry at a later matching use instead of recording bad data.
        const Shader::StageSpecialization current{*info, spec.runtime_info, StageProfile(info->variant), spec.start};
        if (!SotcCache::CanCaptureSpecialization(spec, current)) {
            return false;
        }
        SotcPipeManifest::Stage st;
        st.hash = info->pgm_hash;
        st.variant = info->variant;
        st.spec = ManifestSpecBytes(spec);
        st.hint = SotcPretranslate::BuildFullHint(
            *info, spec.fetch_shader_data.Empty() ? nullptr : &spec.fetch_shader_data);
        st.id = SotcPipeManifest::StageId(st.hash, st.variant, st.spec, st.hint);
        pipe.stages.emplace_back(static_cast<u8>(slot), st.id);
        stages.push_back(std::move(st));
    }
    if (stages.empty()) {
        return true;
    }
    pipe.id = SotcPipeManifest::PipeId(false, pipe.key, pipe.stages);
    if (SotcPipeManifest::Known(pipe.id)) {
        return true;
    }
    for (const auto& st : stages) {
        SotcPipeManifest::AppendStage(header, st);
    }
    SotcPipeManifest::AppendPipe(header, pipe);
    return true;
}

bool PipelineCache::CaptureManifestCompute() {
    const Shader::Info* info = infos[0];
    if (!info || info->sw_stage != Shader::SwStage::Compute) {
        return true;
    }
    const auto found = program_cache.find(info->CacheHash());
    const auto perm = PermOfStage(compute_key.value, info->CacheHash());
    if (found == program_cache.end() || !perm || *perm >= found.value()->modules.size()) {
        return true;
    }
    const auto& spec = found.value()->modules[*perm].spec;
    const Shader::StageSpecialization current{*info, spec.runtime_info, StageProfile(info->variant), spec.start};
    if (!SotcCache::CanCaptureSpecialization(spec, current)) {
        return false; // sotc: do not mix a cached spec with unrelated live descriptor hints
    }
    SotcPipeManifest::Stage st;
    st.hash = info->pgm_hash;
    st.variant = info->variant;
    st.spec = ManifestSpecBytes(spec);
    st.hint = SotcPretranslate::BuildFullHint(*info, nullptr);
    st.id = SotcPipeManifest::StageId(st.hash, st.variant, st.spec, st.hint);
    SotcPipeManifest::Pipe pipe;
    pipe.compute = true;
    pipe.stages.emplace_back(u8{0}, st.id);
    pipe.id = SotcPipeManifest::PipeId(true, pipe.key, pipe.stages);
    if (SotcPipeManifest::Known(pipe.id)) {
        return true;
    }
    SotcPipeManifest::AppendStage(manifest->header, st);
    SotcPipeManifest::AppendPipe(manifest->header, pipe);
    return true;
}

void ReportPipeManifest(std::string& out, double seconds) {
    (void)seconds;
    if (!SotcPipeManifest::On()) {
        return;
    }
    SotcPipeManifest::Flush(); // the records captured in the window (off the command processor)
    static u64 prev_graphics{}, prev_compute{};
    const u64 graphics = g_manifest_stats.late_graphics.load(std::memory_order_relaxed);
    const u64 compute = g_manifest_stats.late_compute.load(std::memory_order_relaxed);
    if (!g_manifest_stats.boot_done.load(std::memory_order_relaxed) ||
        (graphics == prev_graphics && compute == prev_compute)) {
        return;
    }
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC PIPE MANIFEST late: queued %llu graphics + %llu compute after the boot hold (total %llu + "
                  "%llu), stages waiting for code %u, records captured %llu\n",
                  static_cast<unsigned long long>(graphics - prev_graphics),
                  static_cast<unsigned long long>(compute - prev_compute), static_cast<unsigned long long>(graphics),
                  static_cast<unsigned long long>(compute), g_manifest_stats.waiting_code.load(std::memory_order_relaxed),
                  static_cast<unsigned long long>(SotcPipeManifest::Appended()));
    prev_graphics = graphics;
    prev_compute = compute;
    out += line;
}

} // namespace Vulkan
