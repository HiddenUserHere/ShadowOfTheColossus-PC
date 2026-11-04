// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PREDICT_PROBE=1 (off by default; measurement only). See predict_probe.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Shader {
struct Info;
struct RuntimeInfo;
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

/// SOTC_PRETRANSLATE: the number of color layouts known for the pixel program's pass (SOTC_PREDICT_TABLE,
/// SOTC_PREDICT_LAYOUTS), and layout `index` of them into rt (color targets, MRTZ mask, samples). False when the
/// program, its pass or the index has no layout.
size_t PixelLayoutCount(uint64_t hash) noexcept;
bool PixelLayout(uint64_t hash, Shader::RuntimeInfo& rt, size_t index) noexcept;

/// SOTC_PRETRANSLATE: the next OnTranslate calls of this thread are pre-translations (not draws): no layout is
/// learned from them and nothing is counted.
void SetPretranslating(bool on) noexcept;

/// The 5 s report line (driver.cpp); nothing when the switch is off.
void Report(std::string& out, double seconds);

} // namespace SotcPredict
