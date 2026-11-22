// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W08 (shader binds and user data). Shared
// between w08_shaders.cpp and gpu/sotc/hook_gnm_w08.inc. See w08_shaders.cpp for the coverage table.
#pragma once

#include "common/types.h"

// The seven dwords the eboot SET_SH emitter 0x869630 takes as one struct argument. A 28-byte
// aggregate passed by value goes in memory, so the guest places it on the stack above the return
// address; declaring the detour with this type gives the same stack layout under SysV.
struct SotcW08Stack7 {
    u32 v[7];
};
