// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shader_recompiler/specialization.h"

namespace SotcCache {
// A cached module may serve a draw with unbound resources (operator== deliberately masks them).
// Its stored specialization cannot be paired with that draw's different raw descriptor hints.
inline bool CanCaptureSpecialization(const Shader::StageSpecialization& stored,
                                     const Shader::StageSpecialization& current) {
    return stored.bitset == current.bitset && stored == current && current == stored;
}
} // namespace SotcCache
