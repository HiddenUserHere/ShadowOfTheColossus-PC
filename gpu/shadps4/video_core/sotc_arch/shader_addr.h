// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the parts of A3 (SOTC_SHADER_ADDR) that sotc_arch.h does not declare: the per-draw pipeline flag, the skip
// and verify counters, and the report line. Implemented in shader_addr.cpp; Expected and On are in sotc_arch.h.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace VideoCore {
struct Buffer;
}

namespace Shader {
struct Profile;
}

namespace SotcArch::ShaderAddr {

/// SOTC_SHADER_ADDR=2: the old path also runs, and each shader-addressed bind is compared (VerifyBind).
bool Verify();

/// Draw thread, BindResources: the profile of the pipeline about to be bound (nullptr: none). BindBuffers passes it
/// to Shader::ShaderAddrBuffer, which says whether a buffer of that pipeline is addressed by its shader.
void SetBindProfile(const Shader::Profile* profile);
const Shader::Profile* BindProfile();

/// Draw thread, BindBuffers: a shader-addressed guest buffer took no ObtainBuffer and no descriptor.
void NoteSkip(u64 size, bool written);

/// SOTC_SHADER_ADDR=2, BindBuffers: compares Expected(base, size) with the address ObtainBuffer gave the same bind
/// (buffer device address + offset). Counts: match, mismatch (same buffer, other offset), other buffer (a different
/// device memory: not comparable by address), no address, unanswered (Expected 0: the first 8 are listed).
void VerifyBind(VAddr base, u64 size, const VideoCore::Buffer* buffer, u64 offset);

/// One report line into out (NUL-terminated, truncated to size); resets the counters.
void Report(char* out, u32 size, double seconds);

} // namespace SotcArch::ShaderAddr
