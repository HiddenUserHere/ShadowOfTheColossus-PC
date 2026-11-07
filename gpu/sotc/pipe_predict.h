// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PIPE_PREDICT=1 (off by default). Graphics pipelines built from the game's materials before their first
// draw (docs/re-stutters/native/re-pipelines/README.md, step 3c).
//
// The material factory hook (PIPE_PREDICT_MATERIAL, scripts/hooks.d/pipe_predict.py) queues every material the game
// builds. The command processor (PipelineCache::PredictPipelines) takes them with TakeSlots: each pass slot k whose
// stage mask is VS+PS gives (k, VS program hash, PS program hash), read from the OrbShdr footers of the slot's
// programs. The first draw of a pass teaches the rest of that pass's pipeline key (render targets, blend, state:
// one per material pass in SotC); every other material's slot k is then built on a pipeline worker under that key.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SotcPipePredict {

/// SOTC_PIPE_PREDICT=1, read once.
bool On() noexcept;

/// The material factory hook is wanted: SOTC_PIPE_PREDICT=1, or the library prediction of SOTC_GPL_NODROP
/// (SotcNonBlocking::GplPredictRequested, on by default). hooks.cpp turns PIPE_PREDICT_MATERIAL on for the latter,
/// whose own switch (SOTC_PIPE_PREDICT) stays 0. Read once.
bool MaterialsWanted() noexcept;

/// One VS+PS pass slot of a material.
struct Slot {
    uint8_t k{};      ///< the pass slot index (pass-map.md: 0 shadow, 1 depth, 2/3 gbufferbakedambient, ...)
    uint64_t vs{};    ///< vertex program hash (ShaderBinaryInfo::shader_hash, the pipeline cache's program key)
    uint64_t ps{};    ///< pixel program hash
};

/// Command processor: reads up to max_materials queued materials and appends their VS+PS slots to out. A material
/// whose slots hold no program yet is read again on a later call (a few times, then dropped). Returns the number
/// of materials read.
size_t TakeSlots(std::vector<Slot>& out, size_t max_materials);
/// Any thread: materials are queued (no lock; TakeSlots may still find none).
bool HasQueued() noexcept;

/// Counters of the material side, for the report line.
struct MaterialStats {
    uint64_t queued{};    ///< materials the hook queued
    uint64_t read{};      ///< materials read with at least one VS+PS slot
    uint64_t empty{};     ///< materials dropped with no VS+PS slot after the retries
    uint64_t slots{};     ///< VS+PS slots found
    uint64_t unreadable{};///< slots whose program footer was not found
};
MaterialStats Stats() noexcept;

/// Registers the hook handler (hook_handlers.cpp).
void Register();

} // namespace SotcPipePredict
