// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: particle-system count readback probe (plan C7). Hook PARTICLE_COUNT_READ, declared in
// scripts/hooks.d/particle_prev.py, switch SOTC_PARTICLE_PREV (mode 3 only). See particle_prev.cpp.
#pragma once

namespace SotcProbes {

/// Registers the handler of PARTICLE_COUNT_READ (gpu/sotc/hook_handlers.cpp calls it).
void RegisterParticlePrev();

} // namespace SotcProbes
