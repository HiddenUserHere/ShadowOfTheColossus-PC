// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU mirror of the decal visibility mask (sub_19c7e0, site 0x19ce08). Not implemented yet: the compute shader
// is identified (cache 729c541e, two identical variants) but its SRT input (srt_flatbuf, built by the generated SRT
// walker from the four user-data pointers at [set+0x98], [set+0xa8], [set+0xb0], [set+0xd0]) is not mapped to guest
// memory, so the CPU cannot yet reproduce the dword indices it reads (count at srt[49], planes at srt[74 + 4i],
// matrix at srt[54..69]). Until that mapping exists this function reports failure, and the caller keeps serving the
// last completed GPU copy of the decal mask.
#include "cpu_cull.h"

namespace SotcCpuCull {

bool CullDecal(uint64_t, uint64_t, uint32_t, uint32_t*) {
    return false;
}

} // namespace SotcCpuCull
