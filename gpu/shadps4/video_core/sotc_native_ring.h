// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native announce of the ACE (compute) ring dwords that sceGnmDingDong hands to the GPU. The
// engine writes the IB packets into the ring with the CPU (eboot 0x884c40), and the port reads them in
// place (Liverpool::ProcessCompute) after the doorbell. Implemented in gpu/sotc/native/native_hooks.cpp.
#pragma once

#include <cstddef>

namespace SotcNativeRing {

/// Announces [begin, begin + bytes) as PerFrame data the GPU reads from the next submit on. No-op unless
/// SOTC_NATIVE is on. Call after the guest writes and before the ring is handed to the GPU.
void Announce(const void* begin, std::size_t bytes);

} // namespace SotcNativeRing
