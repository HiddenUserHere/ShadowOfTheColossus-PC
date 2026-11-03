// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"

namespace Shader {
struct Profile;
void InjectClipDistanceAttributes(IR::Program& program, RuntimeInfo& runtime_info);
} // namespace Shader

namespace Shader::Optimization {

void SsaRewritePass(IR::Program& program);
void SsaRepairPass(IR::Program& program);
void PhiSimplificationPass(IR::Program& program);
void InverseBallotEliminationPass(IR::Program& program);
/// Blocks outside divergent control flow; uniform_insts are treated as workgroup-uniform.
std::vector<IR::Block*> FindWave64UniformBlocks(
    const IR::Program& program, const std::unordered_set<const IR::Inst*>* uniform_insts = nullptr);
bool Wave64UniformBranchesEnabled();
void SetWave64UniformBranchesDefault(bool enabled);
void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile);
void LowerHardwareIntrinsics(IR::Program& program);
void LowerPhisToRegsPass(IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
void ConstantPropagationPass(IR::BlockList& program);
void FlattenExtendedUserdataPass(IR::Program& program);
void ReadLaneEliminationPass(IR::Program& program);
ResourceDiscoveryList ResourceDiscoverPass(IR::Program& program, const Profile& profile);
void ResourcePatchingPass(Shader::Info& info, const ResourceDiscoveryList& sharp_usages,
                          const Profile& profile);
void CollectShaderInfoPass(IR::Program& program, const Profile& profile);
void LowerBufferFormatToRaw(IR::Program& program);
void LowerUserClipPlanes(IR::Program& program, const RuntimeInfo& runtime_info);
void LowerFp64ToFp32(IR::Program& program);
void RingAccessElimination(const IR::Program& program, const RuntimeInfo& runtime_info);
void TessellationPreprocess(IR::Program& program, RuntimeInfo& runtime_info);
void HullShaderTransform(IR::Program& program, const RuntimeInfo& runtime_info);
void DomainShaderTransform(const IR::Program& program, const RuntimeInfo& runtime_info);
void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile);
void SharedMemorySimplifyPass(IR::Program& program, const Profile& profile);
void SharedMemoryToStoragePass(IR::Program& program, const RuntimeInfo& runtime_info,
                               const Profile& profile);
/// sotc: SOTC_SHADER_EXTENT. Sets BufferResource::sotc_read_extent of the read-only shader-built V#s; IR unchanged.
void SotcShaderExtentPass(IR::Program& program, const RuntimeInfo& runtime_info);
/// sotc: the bytes the loads of one buffer binding can read in `blocks` (0 = not bounded, `reason` says why).
/// workgroup_size bounds LocalInvocationId (null: not a compute shader). Used by the pass and its offline test.
u32 SotcBufferReadExtent(const IR::BlockList& blocks, u32 binding,
                         const std::array<u32, 3>* workgroup_size, std::string* reason);

} // namespace Shader::Optimization
