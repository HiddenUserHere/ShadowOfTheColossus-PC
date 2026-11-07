// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the switches of the non-blocking frame work (docs/re-stutters/native/arch-next/nonblocking-frame.md). Each
// phase is off by default until its measurement gate passes; each is read once.
#pragma once

#include <atomic>
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

} // namespace SotcNonBlocking
