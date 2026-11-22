// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). native_fill: the fill side of the
// material-key -> pipeline cache (gap N2-10, docs/engine/native-render-gaps.md).
//
// Two threads see the same draw. The render thread runs the hooked RenderJob body and derives the
// content key (Native::Bind::BuildKey/HashKey); the command-processor thread is the only one that
// can build a port pipeline, and it builds it from liverpool->regs. Neither side can name the
// other's object, so they meet on the one thing both can see: the positional shader-program
// addresses of the draw (the guest record image dword0, which is exactly what the driver writes to
// SPI_SHADER_PGM_LO).
//
//   render thread (Bind::HashKey):   RegisterPending(SigFromKey(key), digest)
//   command processor (PipelineCache::GetGraphicsPipeline):
//                                    TryRememberSignature(SigFromRegs(regs), pipeline)
//
// RegisterPending only bookkeeps; a pending entry never reaches the cache by itself. A signature
// that names one key resolves to that key's pipeline; a signature shared by several keys is
// ambiguous and resolves to nothing (never guess a pipeline). Everything here is pure host
// bookkeeping, so a wrong guess can only cost a chain to the guest, never a bad draw.
#pragma once

#include <array>

#include "native/native_bind.h"

namespace Native::Bind::Fill {

/// The shader programs of one draw, positionally per GNM ordinal (native_bind.h kNumStages:
/// 0=CS, 1=PS, 2=VS, 3=GS, 4=ES, 5=HS, 6=LS). A slot is the program's 32-bit code VA, or 0 when
/// the stage is absent. Positional (not sorted) on purpose: two draws that differ only by which
/// stage a program lives in must not collide.
struct StageSig {
    std::array<u32, kNumStages> va{};

    bool operator==(const StageSig& o) const noexcept { return va == o.va; }
    bool operator!=(const StageSig& o) const noexcept { return !(*this == o); }
};
static_assert(std::has_unique_object_representations_v<StageSig>);

/// The signature of a derived key: the code VA of every present stage, 0 elsewhere.
StageSig SigFromKey(const MaterialKey& key);

/// Records that the render thread derived `key` for a draw with shaders `sig`. Called from
/// Bind::HashKey (native_bind.cpp). Idempotent for an identical (sig, key) pair; a different key
/// under the same signature is kept as a second entry, so the command processor sees the
/// ambiguity instead of silently picking one.
void RegisterPending(const StageSig& sig, const Key128& key);

/// Resolves a signature to the key's pipeline on the command processor. Unique pending key ->
/// Bind::Remember; no match -> unmatched; several -> ambiguous (nothing remembered).
void TryRememberSignature(const StageSig& sig, const Vulkan::Pipeline* pipeline);

struct Stats {
    u64 registered{0}; ///< RegisterPending calls
    u64 evicted{0};    ///< pending entries dropped by the ring
    u64 attempts{0};   ///< TryRememberSignature calls
    u64 matched{0};    ///< resolved to a unique key and remembered
    u64 ambiguous{0};  ///< several keys share the signature -> nothing remembered
    u64 unmatched{0};  ///< no pending key has this signature
};
Stats StatsSnapshot();

} // namespace Native::Bind::Fill
