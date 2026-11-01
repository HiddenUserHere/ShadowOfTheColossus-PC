// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PC_RING full ring, the copy side (docs/re-stutters/PC-MEMORY-REWRITE.md sections 6-7).
//
// At the start of each top-level gfx submit, on the draw thread and before the first op of that
// submit, the arena pages the game wrote since the previous submit are copied from the imported
// guest memory (SotcGuestMemory) into their arena buffers. Mode 3 applies the ring (the arena
// pages are not write-protected, ApplyRing) and marks the copied pages CPU-clean. Mode 4 only
// records the submit's extents and checks the binds against them (no change in behaviour).
#pragma once

#include "common/types.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {
class BufferCache;
}

namespace VideoCore::PcRingCopy {

/// Draw thread. Queued by ProcessGraphics (liverpool.cpp) for each top-level gfx submit, before
/// the submit's first op. Does nothing when SOTC_PC_RING is off.
void OnSubmitStart(Vulkan::Rasterizer& rasterizer);

/// sotc: SOTC_PC_RING=8, draw thread, from BufferCache::PcHostBind. Once per submit (or when the
/// arena block set changed): imports the live arena blocks for the host binds and applies the ring
/// to the ones that imported (BufferCache::PcHostApply). Does nothing in other modes.
void HostMaintain(BufferCache& cache);

} // namespace VideoCore::PcRingCopy
