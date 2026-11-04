// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PREDICT_PROBE=1 (off by default; measurement only). See predict_probe.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Shader {
struct Info;
struct StageSpecialization;
} // namespace Shader

namespace SotcPredict {

/// SOTC_PREDICT_PROBE=1, read once.
bool On() noexcept;

/// A program the engine's resource loader just decompressed (probe_csdr.cpp, RES_DECOMPRESS_DONE): its footer hash.
void NoteLoaded(uint64_t program_hash) noexcept;

/// A resource the loader hands on (probe_csdr.cpp, RES_LOADED): every program in it is noted as loaded, and each
/// pixel program gets its predicted register state (step 2a).
void NoteLoadedData(const unsigned char* data, size_t n) noexcept;

/// One new translation (vk_pipeline_serialization.cpp RegisterShaderMeta): compares it with what was known before
/// the draw (loaded? pass layout? default resources?).
void OnTranslate(const Shader::Info& info, const Shader::StageSpecialization& spec, size_t perm_idx) noexcept;

/// A pixel shader the warm-up restored from the disk cache (vk_pipeline_serialization.cpp LoadPipelineStage): its
/// color layout joins its pass's set.
void OnCachedProgram(const Shader::Info& info, const Shader::StageSpecialization& spec) noexcept;

/// The 5 s report line (driver.cpp); nothing when the switch is off.
void Report(std::string& out, double seconds);

} // namespace SotcPredict
