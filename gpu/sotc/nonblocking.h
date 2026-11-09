// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the switches of the non-blocking frame work (docs/re-stutters/native/arch-next/nonblocking-frame.md). Each
// phase is off by default until its measurement gate passes; each is read once.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace SotcNonBlocking {

inline bool SwitchOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

/// P1 (SOTC_NB_IO=1): secondary work leaves the frame threads: the driver cache save at the end of the boot hold goes to
/// the saver thread.
inline bool Io() {
    static const bool on = SwitchOn("SOTC_NB_IO");
    return on;
}

/// P1 (SOTC_NB_PREFETCH=1, off): the programs of the pretranslate pack are loaded by a worker when the game announces
/// them, before their first draw. The game announces far more programs than it draws (16,661 queued against ~1,700
/// drawn in a warm session), and every loaded program stays in memory; off, a program is loaded at its first draw.
inline bool Prefetch() {
    static const bool on = SwitchOn("SOTC_NB_PREFETCH");
    return on;
}

/// P2 (SOTC_NB_SCHED=1): the pipeline workers' job lanes run with the modern rules: graphics jobs ordered by demand
/// (draws skipped for them) plus age, cancelled when the command processor built the pipeline itself, background
/// jobs on at most half the workers at below-normal thread priority. Off: the same lanes, FIFO, no cap.
inline bool Sched() {
    static const bool on = SwitchOn("SOTC_NB_SCHED");
    return on;
}

/// P3 (SOTC_NB_GPL=1, with SOTC_PIPE_GPL=1 and SOTC_STAGE_BINDINGS=1): never-wait pipelines. Graphics pipeline layouts
/// are created with independent sets; pre-rasterization and fragment libraries are built per shader (a layout holding
/// only that stage's set; the fragment part without attachment formats). The command processor only links parts that
/// exist (no library build on it); a missing part goes to a worker link job and the draw is skipped meanwhile.
inline bool Gpl() {
    static const bool on = SwitchOn("SOTC_NB_GPL");
    return on;
}

/// SOTC_GPL_NODROP (on unless set to 0): a draw whose graphics pipeline is not built is never dropped. It is recorded
/// with a pipeline fast-linked from libraries (vertex input, pre-rasterization, fragment shader, fragment output; a
/// missing library is built on the command processor, counted); a draw the fast link cannot cover (rect and quad lists,
/// the discard shader with clip emulation, descriptor heap pipelines) builds its full pipeline there instead. A stage
/// still on a translation worker is translated on the command processor. The optimized pipeline keeps building on a
/// worker and replaces the linked one when collected. At the manifest's boot hold the libraries of every manifest
/// pipeline are built too. Also turns on what it needs: the pipeline library extensions (SOTC_PIPE_GPL) and a
/// descriptor set per stage (SOTC_STAGE_BINDINGS, unless set to 0), whose per-shader libraries need independent sets.
/// The request is read from the environment (it decides SOTC_STAGE_BINDINGS, which keys the shader cache before any
/// device exists); the switch is active only when the device reports graphicsPipelineLibrary and
/// graphicsPipelineLibraryFastLinking (SetGplNoDrop, from the instance).
inline bool GplNoDropRequested() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_GPL_NODROP");
        return v == nullptr || v[0] != '0';
    }();
    return on;
}
inline std::atomic<bool>& GplNoDropFlag() {
    static std::atomic<bool> active{false};
    return active;
}
inline void SetGplNoDrop(bool active) {
    GplNoDropFlag().store(active, std::memory_order_relaxed);
}
inline bool GplNoDrop() {
    return GplNoDropFlag().load(std::memory_order_relaxed);
}
/// Per-shader libraries (graphics layouts with independent sets): P3 (SOTC_NB_GPL) or SOTC_GPL_NODROP.
inline bool GplPerShader() {
    return Gpl() || GplNoDrop();
}

/// A switch that is on unless set to 0 (read once by the caller's static).
inline bool SwitchOnByDefault(const char* name) {
    const char* v = std::getenv(name);
    return v == nullptr || v[0] != '0';
}

/// SOTC_GPL_DYN_STATE (on unless set to 0): every pipeline state a pre-rasterization or fragment-shader library would
/// bake is dynamic where the device supports it (VK_EXT_extended_dynamic_state3: polygon mode, depth clamp, depth clip,
/// provoking vertex, clip space [-1,1], rasterization samples; core dynamic primitive topology), set at each draw from
/// the pipeline key. Logged once at device creation ("SOTC DYN STATE"). 0: the former static state.
inline bool DynState() {
    static const bool on = SwitchOnByDefault("SOTC_GPL_DYN_STATE");
    return on;
}
/// Set by the instance when SOTC_GPL_DYN_STATE left no draw state in the shader libraries: a pre-rasterization library
/// is then keyed by its stages and layout only, a fragment library by its pixel stage, sample shading and layout.
inline std::atomic<bool>& GplShaderKeysFlag() {
    static std::atomic<bool> active{false};
    return active;
}
inline void SetGplShaderKeys(bool active) {
    GplShaderKeysFlag().store(active, std::memory_order_relaxed);
}
inline bool GplShaderKeys() {
    return GplShaderKeysFlag().load(std::memory_order_relaxed);
}

/// SOTC_GPL_PREDICT (on unless set to 0, with SOTC_GPL_NODROP; active only with shader-only library keys,
/// GplShaderKeys): the vertex and pixel libraries of the programs a newly created material names (the material factory
/// hook PIPE_PREDICT_MATERIAL, which this request turns on, gpu/sotc/pipe_predict.h) are built on the workers before the
/// material's first draw. With shader-only keys a library is keyed by its stage hash (program and permutation) and
/// layout alone, so any draw of that permutation uses it whatever its state. It was off before shader-only keys (the
/// 7 Nov cold route: 2,749 predicted libraries under a guessed draw state, none used by a draw, private memory 4.1 ->
/// 8.3 GB, warm p50 33 -> 41 ms).
inline bool GplPredictRequested() {
    static const bool on = GplNoDropRequested() && SwitchOnByDefault("SOTC_GPL_PREDICT");
    return on;
}
inline bool GplPredict() {
    return GplPredictRequested() && GplNoDrop() && GplShaderKeys();
}

/// SOTC_GPL_PACK_LIBS (on unless set to 0; with SOTC_GPL_NODROP and shader-only library keys): at the end of the
/// SOTC_PRETRANSLATE loading hold the pre-rasterization library of every translated vertex program and the fragment
/// library of every translated pixel program are compiled on all workers into the driver's pipeline cache, then
/// destroyed (only the material-predicted and manifest libraries stay in memory; they are created again from the cache
/// when their area loads). The compiled set is listed in sotc_pack_libraries.bin (versioned cache folder, with the
/// driver identity), so a later launch compiles only what the list lacks.
inline bool GplPackLibs() {
    static const bool on = SwitchOnByDefault("SOTC_GPL_PACK_LIBS");
    return on && GplNoDrop() && GplShaderKeys();
}

/// SOTC_GPL_PARALLEL_LIBS (on unless set to 0; with SOTC_GPL_NODROP): a draw whose vertex and pixel libraries are both
/// missing queues the two builds at the front of the workers' urgent lane and waits for them (the command processor
/// takes a build no worker has taken yet), instead of building them one after the other itself.
inline bool GplParallelLibs() {
    static const bool on = SwitchOnByDefault("SOTC_GPL_PARALLEL_LIBS");
    return on && GplNoDrop();
}

/// SOTC_GPL_LIB_PHASE (on unless set to 0; with SOTC_GPL_NODROP): the first manifest boot hold builds the libraries
/// after the translations and pipelines, and samples the process private memory between the two phases, so the
/// "SOTC GPL NODROP" boot line splits the growth into pipelines + translations and libraries.
inline bool GplLibPhase() {
    static const bool on = SwitchOnByDefault("SOTC_GPL_LIB_PHASE");
    return on;
}

/// P4 (SOTC_NB_WARM=1, with SOTC_NB_GPL): libraries of new vertex and pixel shaders built ahead of their first draw on
/// background workers. Measured slower on the first session (the warm-up takes workers and CPU the frame needs); off.
inline bool Warm() {
    static const bool on = SwitchOn("SOTC_NB_WARM");
    return on;
}

/// SOTC_NB_WORKER_CAP=1 (independent of the phases above): the pipeline workers spend less CPU during play. Jobs no draw
/// or recorder waits for on this frame (the Shadow, Graphics and Background lanes) run on at most a quarter of the
/// hardware threads (at least 2) at below-normal priority; Compute, Translate and Tiny jobs, which the recorder or a
/// draw waits for, are not capped. New pipelines and objects may appear a few frames later. Off: no cap, as before.
inline bool WorkerCap() {
    static const bool on = SwitchOn("SOTC_NB_WORKER_CAP");
    return on;
}

/// SOTC_NB_LAZY_PSO=1: the boot warm-up builds only the saved pipelines used in the last 3 sessions (the list is
/// sotc_recent_pso.bin in the versioned cache folder), not all of them (~6,200 pipelines and ~7,500 programs resident
/// from the start); the rest are built or loaded at their first draw or dispatch, from the driver's own disk cache.
/// The first session after enabling it has no list yet and builds none at boot.
inline bool LazyPso() {
    static const bool on = SwitchOn("SOTC_NB_LAZY_PSO");
    return on;
}

/// SOTC_GPL_PACK_LEAN (on unless set to 0; with SOTC_GPL_PACK_LIBS): the first-launch pack compile runs lean: after the
/// manifest pipelines and the compute prebuilds, on every hardware thread (extra workers for the hold only), each pack
/// job's shader module is destroyed as soon as its library is compiled (the program keeps its hash, permutation and
/// specialization in the shader index and is loaded again from the pack at its first draw), and the driver cache and
/// the compiled list are written once at the end, on the saver thread (the list after the cache, so it never names a
/// library the saved cache lacks); the hold waits for that write only while it is short. 0: the 7 Nov behaviour (every
/// module resident until the eviction, the normal worker count, a synchronous save on the command processor).
inline bool GplPackLean() {
    static const bool on = SwitchOnByDefault("SOTC_GPL_PACK_LEAN");
    return on;
}

/// SOTC_COMPUTE_PACK (on unless set to 0; with SOTC_GPL_PACK_LEAN): the first-launch loading hold also compiles every
/// compute program of the loaded shader files (the SOTC_PRETRANSLATE_COMPUTE prebuild, turned on by this switch when
/// its own is unset: the hint of user/shader_predict/compute_hints.tsv when there is one, the synthetic perm-0
/// specialization otherwise) into the pack's VkPipelineCache (sotc_vk_pack.bin), then destroys the pipeline and its
/// module (the program stays in the shader index and is loaded again at its first dispatch), except for the pipelines
/// a dispatch already bound (resident as before). Their ids go into sotc_pack_libraries.bin with the compute kind bit,
/// so a later launch skips them, and a dispatch whose pipeline the list names is built inline by the recorder from
/// the pack cache (a ~1 ms hit) instead of waiting for a worker. The manifest's compute pipelines are not touched.
/// ComputePackRequested is the switch alone (read once); ComputePack adds the runtime gate of the GPL pack.
/// sotc: default off since 8 Nov: 3,799 of 3,800 packed programs had no hint (never dispatched in any recorded session), so their synthetic specs never matched a dispatch; 61 s of first loading for nothing.
inline bool ComputePackRequested() {
    static const bool on = SwitchOn("SOTC_COMPUTE_PACK") && GplPackLean();
    return on;
}
inline bool ComputePack() {
    return ComputePackRequested() && GplPackLibs();
}

/// sotc: SOTC_COMPUTE_FAST=1 (default off). A compute pipeline a dispatch needs and that is not built yet gets a first
/// build with VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT on the Urgent lane (or on the recorder when it would wait),
/// so the recorder waits only for that; the optimized build follows on the Background lane and replaces the handle
/// (the fast one is destroyed once the GPU finished its ticks). With the device's cache control, the optimized build
/// is first probed in the driver cache (FAIL_ON_PIPELINE_COMPILE_REQUIRED): a hit is used directly. Pipelines prebuilt
/// by the manifest, the compute prebuilds and pack-listed ones keep the normal build (ComputePipeline, vk_pipeline_cache).
inline bool ComputeFast() {
    static const bool on = SwitchOn("SOTC_COMPUTE_FAST");
    return on;
}

/// SOTC_COMPUTE_PACK_MAX=<n> (default 10000): the most compute programs the compute pack prebuilds in one session; a
/// shader set beyond it is capped (the rest build at their first dispatch, as before) and the pack line says so.
inline unsigned ComputePackMax() {
    static const unsigned n = [] {
        const char* v = std::getenv("SOTC_COMPUTE_PACK_MAX");
        if (v == nullptr || v[0] == '\0') {
            return 10000u;
        }
        const long parsed = std::strtol(v, nullptr, 10);
        return parsed < 0 ? 0u : static_cast<unsigned>(parsed);
    }();
    return n;
}

/// SOTC_GPL_URGENT_WORKERS=<n> (default 2; 0 off): this many pipeline workers serve only the Urgent and Predict lanes
/// (the library builds a draw waits for, and the libraries of the materials the game just created), so a library
/// compile costs its real time on an idle worker, never a turn behind translations, compute prebuilds and optimized
/// pipeline builds. The other workers serve every lane as before. Needs at least n + 2 workers, else none is reserved.
inline unsigned GplUrgentWorkers() {
    static const unsigned n = [] {
        const char* v = std::getenv("SOTC_GPL_URGENT_WORKERS");
        if (v == nullptr || v[0] == '\0') {
            return 2u;
        }
        const long parsed = std::strtol(v, nullptr, 10);
        return parsed < 0 ? 0u : static_cast<unsigned>(parsed);
    }();
    return n;
}

/// SOTC_PLAY_QUIET (on unless set to 0): nothing heavy while the game plays. The manifest's later holds and the
/// pretranslate hold run only while the loading screen is shown (late manifest stages go to the Predict lane during
/// play); every driver cache save runs on the saver thread (never the command processor, draw or a guest thread), at a
/// loading screen, at exit, or after the cache has seen no pipeline creation for a few seconds; and the driver cache is
/// split in two VkPipelineCache objects: the pack cache (stage libraries: pack, predicted, manifest, urgent; saved by
/// the pack publication) and the live cache (every other pipeline; the periodic saver reads only this one).
inline bool PlayQuiet() {
    static const bool on = SwitchOnByDefault("SOTC_PLAY_QUIET");
    return on;
}

// sotc: plan C (command processor thread costs, docs/re-stutters/LOCKED-30-PLAN-2026-11-08.md).

/// SOTC_MANIFEST_HOLD_FAST (on unless set to 0): the manifest hold check on every slow pipeline lookup is O(1): the
/// Pending stage count is kept at each stage state change (no pass over the ~2,700 stage records per lookup), the pump
/// skips its pass when no stage is Pending and no new code came, and a compute lookup only installs finished
/// translations. =0 restores the per-lookup passes.
inline bool ManifestHoldFast() {
    static const bool on = SwitchOnByDefault("SOTC_MANIFEST_HOLD_FAST");
    return on;
}

/// SOTC_BACK_WRITE_EXACT=1 (off): the submit-start ops queued to the draw thread, which write no guest memory
/// (PcRingCopy::OnSubmitStart, BufferCache::DrainSyncEvents, Rasterizer::GcnSubmitStart: uploads, imports, residency,
/// protections), no longer advance back_write_seq, so a CP pipeline lookup after them does not wait for the draw thread
/// (LookupAfterWrites with SOTC_CP_PRECISE_WAIT=0).
inline bool BackWriteExact() {
    static const bool on = SwitchOn("SOTC_BACK_WRITE_EXACT");
    return on;
}

/// SOTC_CP_LABEL_SLEEP=1 (off, Windows): after 64 command processor rounds over the queues with no progress (every task
/// waits on a label, a register/memory value or a counter), the thread sleeps on cp_wake_epoch (WaitOnAddress, 1 ms
/// timeout) instead of yielding; NotifyCpWait wakes it (the draw thread after each queued function, new submits and
/// commands, labels and fences written). SOTC_CP_LABEL_SLEEP_ROUNDS / SOTC_CP_LABEL_SLEEP_MS tune it.
inline bool CpLabelSleep() {
    static const bool on = SwitchOn("SOTC_CP_LABEL_SLEEP");
    return on;
}

/// SOTC_RING8_CACHE=1 (off): the label lookahead of a submit (Ring8ScanSubmit) is cached per (DCB, CCB) span with a copy
/// of every buffer it walked; a later submit of the same spans takes it while the memory table generation is unchanged
/// and every buffer still holds the copied words (the game rewrites command buffers in place without a table change).
inline bool Ring8Cache() {
    static const bool on = SwitchOn("SOTC_RING8_CACHE");
    return on;
}

/// Plan B of docs/re-stutters/LOCKED-30-PLAN-2026-11-08.md: the draw thread's per-draw work, one switch per item, all
/// off by default (each is measured alone on the test route). Counters: SotcStats::pb_* (sotc_stats.h), printed in the
/// "SOTC PLAN-B" 5 s line (vk_rasterizer.cpp) while any of them is on.

/// SOTC_BIND_DESC_REUSE=1: a frame bind-cache hit whose entry holds the descriptor sets committed for its last write
/// binds those sets again (no DescriptorHeap::Commit, no vkUpdateDescriptorSets) when they are still valid. With the
/// layout cache on (SOTC_STAGE_BINDINGS=1 or SOTC_LAYOUT_CACHE=1) the entry's sets are persistent: allocated from pools
/// that are never reset (DescriptorHeap::AcquirePersistent), owned by the entry and freed only after the GPU finished
/// the last tick that bound them; without it they come from the rolling pool and hold until it rolls (PoolSerial).
/// The flat buffer bindings of pool-allocated sets are dynamic storage buffers (Pipeline::MarkDynamicFlat; changes
/// the set and pipeline layouts, not the SPIR-V), so a flat copy at a new stream-ring offset is a dynamic offset; a
/// copy in another buffer (arena <-> ring) writes the sets again. A layout whose flat buffers exceed the device's
/// dynamic storage buffer limit keeps them static (counted). The rolling pool is 8x larger with the switch. Classic
/// set paths only (per-stage sets or set 0); push descriptors, descriptor buffers and heap pipelines keep no sets.
inline bool BindDescReuse() {
    static const bool on = SwitchOn("SOTC_BIND_DESC_REUSE");
    return on;
}

/// SOTC_SAMPLER_CACHE=1: the per-tick sampler re-resolution of a bind-cache hit (TextureCache::GetSampler per sampler,
/// a mutex each) runs only every 8 ticks per entry; a destroyed sampler flushes the whole cache anyway
/// (SamplerGeneration), so the handles cannot change in between. The periodic run keeps the samplers' LRU touched.
inline bool SamplerCache() {
    static const bool on = SwitchOn("SOTC_SAMPLER_CACHE");
    return on;
}

/// SOTC_HLE_FILL_SKIP=1: a REPLICATE fill (vk_shader_hle.cpp) whose control words, pattern hash and generation sums
/// (SotcGen::Sum of the destination, source and control ranges) are those of the last fill of the same destination,
/// with none of the three ranges CPU- or GPU-modified, skips the page compare and the write-back: the destination
/// already holds this table. Needs a generation source (SOTC_GEN_BIND or SOTC_GEN_DMA) and page tracking of guest
/// writes (not SOTC_NATIVE without tracking); otherwise every fill runs as before and is counted as not skippable.
inline bool HleFillSkip() {
    static const bool on = SwitchOn("SOTC_HLE_FILL_SKIP");
    return on;
}

/// SOTC_FLAT_ARENA=1: flat constant buffers go to a persistent arena keyed by content hash (BufferCache::FlatArena)
/// instead of the stream ring: a table seen before is bound at its arena offset (no copy); a new one is uploaded
/// once. The arena is 32 chunks of 2 MiB bump-allocated; a chunk unused for 64 ticks and finished by the GPU is
/// emptied and reused (its entries retired). When no chunk is free the copy goes to the stream ring as before.
inline bool FlatArena() {
    static const bool on = SwitchOn("SOTC_FLAT_ARENA");
    return on;
}

/// SOTC_PROTECT_ASYNC_UNPROTECT=1 (with SOTC_PROTECT_ASYNC=1): an unprotect (read-write) the draw thread makes while
/// it binds resources for the GPU (PageManager::GpuOnlyScope, opened by Rasterizer::BindResources) is queued for the
/// protect worker like a restriction, in order, instead of flushing the queue and running synchronously. Outside the
/// scope (HLE fills, readback write-back, faults) unprotects stay synchronous. A guest write that meets a page whose
/// unprotect is still queued faults and the handler applies the tracker's state (EnsurePageWritable), as it does for
/// a page covered by another thread's pending flush.
inline bool ProtectAsyncUnprotect() {
    static const bool on = SwitchOn("SOTC_PROTECT_ASYNC_UNPROTECT");
    return on;
}

/// SOTC_FILL_GPU=1 (off by default): the replicate fill of a table (vk_shader_hle.cpp FillTableGpu) writes the guest
/// bytes inline and fills the arena on the GPU, with no page compare or per-run replace. Read once.
inline bool FillGpu() {
    static const bool on = SwitchOn("SOTC_FILL_GPU");
    return on;
}

/// SOTC_BIND_CACHE_WIDE=1 (default off; needs SOTC_BIND_CACHE, _RANGES, _XTICK and _FRAME, read here on their own): the
/// frame bind cache also takes compute dispatches and binds that write plain guest buffers, without SOTC_OP_COMPUTE_BIND
/// being set. It applies the rules of SOTC_OP_COMPUTE_BIND=1 (vk_rasterizer.cpp, OpComputeMode): a hit replays the written
/// bindings' side effects (Rasterizer::ReplayOwnWrites). Storage images, fmasks and written images stay refused, counted by
/// gate in the "SOTC BIND WIDE" 5 s line. With SOTC_OP_COMPUTE_BIND=2 set as well, the verify mode runs instead of hits.
inline bool BindCacheWide() {
    static const bool on = [] {
        const auto one = [](const char* name) {
            const char* v = std::getenv(name);
            return v != nullptr && v[0] == '1';
        };
        return one("SOTC_BIND_CACHE_WIDE") && one("SOTC_BIND_CACHE") && one("SOTC_BIND_CACHE_RANGES") &&
               one("SOTC_BIND_CACHE_XTICK") && one("SOTC_BIND_CACHE_FRAME");
    }();
    return on;
}

// sotc: SOTC_WATCH_SKIP=1 (off by default; read once). Skips page-watcher and CPU-walk work the GPU-only protection
// policy does not need: write-watcher changes of a page-tracker update when SOTC_NATIVE_TRACK=3 with
// SOTC_NATIVE_TRACK_GPU_ONLY=1 (PageManager::UpdatePageWatchersForRegion), and the locked CPU-modified walk of a region
// whose 2 MiB chunks hold no CPU bit (RegionManager::cpu_chunks). Both are exact; the counters below say how often.
inline bool WatchSkip() {
    static const bool on = SwitchOn("SOTC_WATCH_SKIP");
    return on;
}

/// sotc: SOTC_WATCH_SKIP counters, cumulative since start (the 5 s line reads them and may diff them).
/// gpu_only_skips: page-watcher updates dropped (write-only calls skipped). clean_skips: CPU-modified queries answered
/// by a clean chunk summary. walks / pages_walked: locked CPU-modified walks and the pages they covered.
namespace WatchSkipStats {
inline std::atomic<uint64_t> gpu_only_skips{0};
inline std::atomic<uint64_t> clean_skips{0};
inline std::atomic<uint64_t> walks{0};
inline std::atomic<uint64_t> pages_walked{0};
} // namespace WatchSkipStats

/// SOTC_PACK_REFERENCED (off unless set to 1; with SOTC_GPL_PACK_LIBS): the first-loading pack compiles the stage
/// libraries of the programs a material the game has created so far names (the material factory hook,
/// gpu/sotc/pipe_predict.h) and of the manifest's stages, not of every loaded program. A material created after that
/// pack queues its libraries on the Predict lane when the command processor sees it; each one built is listed for the
/// next launch by the exit publication.
inline bool GplPackReferenced() {
    static const bool on = SwitchOn("SOTC_PACK_REFERENCED");
    return on;
}

/// sotc: design E (docs/re-stutters/DESIGN-NEXT-CUTS-2026-11-08.md section 2). A mode switch: 0 when unset or "0",
/// 1 or 2 by its first digit (other values read as 1).
inline int ModeSwitch(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0' || v[0] == '0') {
        return 0;
    }
    return v[0] == '2' ? 2 : 1;
}

/// E1 (SOTC_PR_PRED, off; needs SOTC_PARALLEL_RECORD=1). The GpuPrep thread computes the buffer predicates of every stage
/// of a prepared op (shader-addressed, special, written; PreparedDraw::pred_*, Rasterizer::PreparePredicates) and
/// BindBuffers reads the shader-addressed bit from the slot instead of ShaderAddrBufferAt. 2: verify, BindBuffers
/// computes the bits itself, uses its own, and counts every difference ("pred diff" in the SOTC PREP line).
inline int PrPred() {
    static const int mode = ModeSwitch("SOTC_PR_PRED");
    return mode;
}

/// E2 (SOTC_PR_BCPROBE, off; needs SOTC_PARALLEL_RECORD=1 and SOTC_BIND_CACHE_FRAME). The GpuPrep thread computes the
/// frame bind-cache key of a prepared draw (with the render-target key the draw thread published last) and probes the
/// table read-only: the entry index and its sequence number, or the table's commit sequence for a miss
/// (Rasterizer::BindCacheProbeFrame). The draw thread checks the probe (pipeline, hash, targets key, index, sequence,
/// key) and then runs TryBindCacheFrame without its own key computation and lookup; a stale probe falls back to the
/// lookup. 2: verify, the lookup runs too and is the one used; differences are counted ("bcprobe mismatch").
inline int PrBcProbe() {
    static const int mode = ModeSwitch("SOTC_PR_BCPROBE");
    return mode;
}

} // namespace SotcNonBlocking
