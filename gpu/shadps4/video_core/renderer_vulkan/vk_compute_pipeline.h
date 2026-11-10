// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional> // sotc: SOTC_COMPUTE_FAST (QueueOptimizedHook)
#include <mutex>
#include <chrono>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Serialization {
struct Archive;
}

namespace Vulkan {

class Instance;
class Scheduler;
class DescriptorHeap;

/// sotc: prints "SOTC SLOW BUILD <kind> <ms> ms" when start was SOTC_SLOW_BUILD_MS (default 500)
/// or more ago (vk_pipeline_cache.cpp).
void LogSlowBuild(const char* kind, std::chrono::steady_clock::time_point start);

struct ComputePipelineKey {
    size_t value;

    friend bool operator==(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return lhs.value == rhs.value;
    }
    friend bool operator!=(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return !(lhs == rhs);
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

/// sotc: SOTC_COMPUTE_SPECFREE counters (totals; ReportComputeSpecFree in vk_compute_pipeline.cpp prints them every
/// 5 s). dispatches/hits: dispatches, and those whose pipeline was not compiled by a dispatch lookup (prebuilt at
/// shader file load, warmed up, or in the pack cache). fallback_*: new compute permutations of a program that had a
/// prebuilt one, by the first key part that differs from it (image dimension, image integer type, a written formatted
/// buffer, an atomic image, anything else); not_prebuilt: compute programs first translated by a dispatch.
/// programs/variants: prebuilds installed (programs, and their permutations with the int/float variants);
/// compile_ns: worker time of their translations and pipeline builds.
/// sotc: perms_hist: prebuilt programs by the permutations queued for them (1, 2, 4, 8, over the cap: the two
/// all-int/all-float ones of a program with more than kSpecFreeMaxAmbiguous ambiguous images); perms_queued: all
/// those permutations (the expected prebuild count); fallback_int_capped: the int fallbacks of over-cap programs.
struct ComputeSpecFreeStats {
    std::atomic<u64> dispatches{0}, hits{0};
    std::atomic<u64> fallback_dim{0}, fallback_int{0}, fallback_written_fmt{0}, fallback_atomic{0},
        fallback_other{0};
    std::atomic<u64> fallback_int_capped{0};
    std::atomic<u64> perms_hist[5]{};
    std::atomic<u64> perms_queued{0};
    std::atomic<u64> not_prebuilt{0};
    std::atomic<u64> programs{0}, variants{0}, compile_ns{0};
    /// sotc: SOTC_SHADER_ADDR (table mode): compute translations the CP routed to a worker and waited for
    /// (TranslateComputeOnWorker), the CP's wait time, waits that timed out, and translations still made inline on the
    /// CP (a worker failure or timeout). Always 0 in descriptor mode.
    std::atomic<u64> cp_worker{0}, cp_worker_ns{0}, cp_worker_timeouts{0}, cp_inline{0};
    /// sotc: SOTC_HEAP: prebuilt programs installed as the heap variant, and heap candidates the translated Info refused
    /// (prebuilt again as the plain variant). Always 0 with the heap off.
    std::atomic<u64> heap_prebuilds{0}, heap_refused_prebuilds{0};
    std::atomic<u32> logged{0};
};
extern ComputeSpecFreeStats g_specfree;

class ComputePipeline : public Pipeline {
public:
    struct SerializationSupport {
        u32 dummy{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    /// sotc: deferred: only the layouts are made here; Build makes the pipeline on a pipeline
    /// worker (SOTC_ASYNC_COMPUTE) and command buffers bind it with BindWhenReady.
    ComputePipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                    LayoutCache& layout_cache, const Shader::Profile& profile,
                    vk::PipelineCache pipeline_cache,
                    ComputePipelineKey compute_key, const Shader::Info& info,
                    vk::ShaderModule module, SerializationSupport& sdata, bool preloading,
                    bool deferred = false);
    ~ComputePipeline();

    /// sotc: makes the pipeline of a deferred construction, unless another thread claimed it
    /// first (pipeline worker, or the recording thread when it needs it before any worker
    /// started it: workers may be waiting for GPU work queued behind that bind, since shader
    /// translation reads guest memory on them).
    void Build();

    /// sotc: binds the pipeline in the current command buffer. A deferred pipeline still being
    /// built is bound on the recording thread, which waits for it there, so the command
    /// processor goes on (and other new pipelines build meanwhile).
    /// sotc: SOTC_N5_ASYNC_COMPUTE: the pipeline is built, so its bind is recorded right away (a
    /// bind that is still deferred would land in the graphics command buffer).
    [[nodiscard]] bool IsReady() const noexcept {
        return ready.load(std::memory_order_acquire);
    }

    void BindWhenReady() const;

    /// sotc: SOTC_COMPUTE_FAST (SotcNonBlocking::ComputeFast). Claims a deferred construction and makes its first
    /// pipeline: the optimized one when the driver cache has it (probe, with the device's cache control), otherwise a
    /// build with DISABLE_OPTIMIZATION. True when the optimized build must still follow (BuildOptimized, queued by the
    /// caller on a background lane); false when it was claimed already or the optimized pipeline is in place.
    bool BuildFast();

    /// sotc: SOTC_COMPUTE_FAST: the optimized build after BuildFast; it replaces the fast handle for later binds and
    /// retires the fast one by tick (Install).
    void BuildOptimized();

    /// sotc: SOTC_COMPUTE_FAST: the bound handle is the fast (unoptimized) build still. Such a pipeline is not moved to
    /// the async compute queue (its retirement waits for graphics ticks only).
    [[nodiscard]] bool OnFastBuild() const noexcept {
        return sotc_fast_live.load(std::memory_order_acquire);
    }

    /// sotc: SOTC_COMPUTE_FAST: queues BuildOptimized for a pipeline on a background lane (set by the PipelineCache that
    /// owns the workers; used when the recorder made the fast build itself).
    static inline std::function<void(ComputePipeline*)> QueueOptimizedHook;

    /// sotc: waits until a deferred construction has made its pipeline (building it here if no
    /// worker started it yet). Handle() is then the result, null if the driver refused it.
    void WaitBuilt();

    /// sotc: N5. Draws that bound this pipeline (command processor only), and the count its blob
    /// holds (saved_use_count), as for GraphicsPipeline.
    mutable u32 use_count{};
    u32 saved_use_count{};
    /// sotc: SOTC_NB_LAZY_PSO: already noted in the recently used list this session.
    mutable bool recent_noted{};
    /// sotc: SOTC_PIPE_MANIFEST: already checked against the manifest (captured when it held none) this session.
    mutable bool manifest_noted{};
    /// sotc: SOTC_COMPUTE_PACK: a prebuild of the loading hold, compiled into the pack cache; the compute pack destroys
    /// it (and its program's module) once built unless a dispatch bound it. sotc_pack_program is its program cache key.
    bool sotc_pack{};
    u64 sotc_pack_program{};
    /// sotc: SOTC_COMPUTE_PACK: the pack list names this pipeline (a driver-cache hit): no worker job is queued, the
    /// recorder builds it inline at its first bind (counted as an inline build, not a wait).
    bool sotc_inline{};
    /// sotc: SOTC_COMPUTE_SPECFREE: compiled by a dispatch's lookup (not a prebuild, not from the pack cache).
    bool sotc_dispatch_built{};
    /// sotc: SOTC_COMPILE_STREAM: rebuilt from the pack cache by StreamRehydrate (its first lookup counts a late
    /// rehydrate when it is not built yet, and clears this).
    bool sotc_rehydrated{};
    /// sotc: SOTC_COMPILE_STREAM: the worker that built this prebuild no longer touches it (set last by the worker),
    /// so the command processor may destroy it during the hold.
    std::atomic<bool> sotc_worker_done{false};
    u64 KeyValue() const {
        return compute_key.value;
    }

    /// sotc: SOTC_COMPUTE_RELEASE: the module is no longer read: the optimized pipeline is built and installed (no
    /// SOTC_COMPUTE_FAST build is live or running, so no BuildOptimized or BuildFastBeside follows).
    [[nodiscard]] bool ModuleReleasable() const noexcept {
        return IsReady() && compute_handle.load(std::memory_order_acquire) != VkPipeline{} &&
               !sotc_fast_live.load(std::memory_order_acquire) && !sotc_fast_running.load(std::memory_order_acquire);
    }
    /// sotc: SOTC_COMPUTE_RELEASE: forgets the module handle (the owner destroys it). Only after ModuleReleasable.
    void ReleaseModule() noexcept {
        module = vk::ShaderModule{};
    }

private:
    /// sotc: SOTC_COMPUTE_FAST: Normal is the build as before; Fast adds DISABLE_OPTIMIZATION; Probe asks the driver
    /// cache for the optimized pipeline (FAIL_ON_PIPELINE_COMPILE_REQUIRED, no compile).
    enum class BuildMode { Normal, Fast, Probe };
    void CreatePipeline();
    /// sotc: one create call; the result and the pipeline (null on failure). Asserts only in Normal mode.
    std::pair<vk::Result, vk::UniquePipeline> MakePipeline(BuildMode mode);
    /// sotc: SOTC_COMPUTE_FAST: installs a built pipeline (sotc_swap_mutex). The first one becomes the handle; an
    /// optimized one replaces a fast one (whose destruction is deferred until the GPU reached the current tick); any
    /// other is destroyed at once (never bound).
    bool Install(vk::UniquePipeline built, bool optimized);
    /// sotc: SOTC_COMPUTE_FAST: the recorder's fast build while a worker builds the optimized one (no claim).
    void BuildFastBeside();

    ComputePipelineKey compute_key;
    vk::PipelineCache cache;
    vk::ShaderModule module;
    bool from_cache{}; ///< sotc: preloaded from the cache (a refused one is dropped)
    std::atomic<bool> ready{true};
    std::atomic<bool> claimed{false};
    /// sotc: SOTC_PIPE_COMPILE_CHECK: set once this pipeline was logged (waited for or built off a worker).
    mutable std::atomic<bool> sotc_logged{false};
    /// sotc: SOTC_COMPUTE_FAST state: the swap lock, the fast handle is bound, a fast build is running, the claimed build
    /// is the normal (optimized) one, and the fast build's time (ns, for the evidence line).
    std::mutex sotc_swap_mutex;
    std::atomic<bool> sotc_fast_live{false};
    std::atomic<bool> sotc_fast_running{false};
    std::atomic<bool> sotc_normal_claim{false};
    std::atomic<u64> sotc_fast_ns{0};
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::ComputePipelineKey> {
    std::size_t operator()(const Vulkan::ComputePipelineKey& key) const noexcept {
        return std::hash<size_t>{}(key.value);
    }
};
