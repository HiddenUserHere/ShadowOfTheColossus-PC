// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_TEX_REGISTER (docs/re-stutters/native/GCN-NATIVE-PLAN-2026-11-09.md, step 2). The engine side of the
// S3 registration: hook handlers that read the T#s the engine owns (material blocks, render targets, pass textures,
// in-place T# rewrites) and queue them for the T# table (SotcGcn::Tex::QueueRegister / QueueRefresh, gcn_native.h).
// Hooks: scripts/hooks.d/gcn_tex_register.py. The material factory hook is shared with pipe_predict.cpp
// (PIPE_PREDICT_MATERIAL), which forwards each material here.
#pragma once

#include <cstdint>

namespace SotcGcnTexRegister {

/// The material source is requested (environment only, read once): hooks.cpp turns PIPE_PREDICT_MATERIAL on for it.
bool MaterialsWanted() noexcept;

/// PIPE_PREDICT_MATERIAL post phase (game loader thread): material = the factory's result (non-zero). Reads the T#s of
/// both material blocks (the block data and the list payloads' CTexture+0xc4) and queues them.
void OnMaterial(uintptr_t material);

/// Registers the hook handlers (hook_handlers.cpp).
void Register();

} // namespace SotcGcnTexRegister
