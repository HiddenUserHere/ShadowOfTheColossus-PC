// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native particle stats (docs/re-stutters/native/impl-particles.md, option 3, approved 1 Nov):
// ParticleSystemPool::ProcessStats reads the last completed copy of its GPU count array instead of the
// GPU-written array, so the CPU never reads GPU memory there and never waits for the GPU.
//
// Switch SOTC_NATIVE_PARTICLES=prev (default off: the exact read). Needs SOTC_NATIVE (the frame ticket);
// sotc.py sets SOTC_PARTICLE_PREV=3, which installs the stats hook of scripts/hooks.d/particle_prev.py in
// serving mode. SOTC_NATIVE=2 / SOTC_VERIFY=1: nothing is served; the copy is compared with the exact read.
#pragma once

#include <cstdint>
#include <string>

#include "../hooks.h"
#include "video_core/renderer_vulkan/vk_common.h" // sotc: vk::CommandBuffer, vk::Buffer

namespace VideoCore {
struct Buffer;
}

namespace SotcNativeParticles {

/// SOTC_NATIVE_PARTICLES=prev, read once.
bool Armed();

/// Bytes of the host-readable copy ring the rasterizer creates (Attach).
inline constexpr uint64_t kCopyRingBytes = 2ull << 20;

/// One ProcessStats call, at the entry of the array accessor 0x8c1b0 (game thread, particle_prev.cpp).
/// counter = pool+0x64 after this call's increment; read_base = the array the call reads (dispatched
/// two calls ago); dispatch_base / dispatched = the array this call's count pass writes and its u32
/// entries; extent = one past the highest system index the loop reads. Registers the dispatched array
/// for a copy, then either returns HookAction::Return with regs.rax = a guest block holding the last
/// completed counts (served), or Continue (the exact read).
SotcHooks::HookAction OnStatsRead(SotcHooks::GuestRegs& regs, uint64_t pool, uint64_t counter,
                                  uint64_t read_base, uint64_t dispatch_base, uint64_t dispatched,
                                  uint64_t extent);

/// Registers the handler of NATIVE_PARTICLE_VERIFY (the site after the count load at 0x101a98).
void Register();

/// Draw thread. The rasterizer's host-readable copy buffer (nullptr detaches it).
void Attach(VideoCore::Buffer* staging);
/// Draw thread, after a dispatch: a buffer range the dispatch wrote. When it overlaps a registered
/// array, records the GPU copy of the overlap into the staging buffer.
void NoteWrittenBind(vk::CommandBuffer cmdbuf, vk::Buffer src, uint64_t src_offset, uint64_t addr,
                     uint64_t size);
/// True while arrays wait for copies (cheap check before walking the written binds).
bool Watching();

/// "SOTC NATIVE_PARTICLES" 5 s line (counters reset). Nothing when not armed.
void Report(std::string& out, double seconds);

} // namespace SotcNativeParticles
