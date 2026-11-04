// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_COMMIT (S6, decisions D13-D15). The commit lane: an op whose pipeline is commit-eligible skips the bind
// caches and binds directly (vk_rasterizer.cpp, BindResources), and dispatches are prepared on the GpuPrep threads.
// This file holds the switch's helpers, the eligibility of a pipeline and the counters. gcn_native.h, namespace Commit.
//
// Modes: 0 off. 1 on (eligible ops bypass the caches). 2 verify (the caches run as before; eligible ops are counted).
#include <atomic>
#include <cstdio>

#include "common/types.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/gcn_rules.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/sotc_arch/gcn_native.h"

namespace SotcGcn::Commit {

namespace {

/// Counters of the 5 s report (relaxed; read and reset by Report).
struct Counters {
    std::atomic<u64> ops{};
    std::atomic<u64> eligible_ops{};
    std::atomic<u64> draws{};
    std::atomic<u64> eligible_draws{};
    std::atomic<u64> dispatches{};
    std::atomic<u64> eligible_dispatches{};
    std::atomic<u64> bypass{};
    std::atomic<u64> dispatch_taken{};
    std::atomic<u64> dispatch_not{};
    std::atomic<u64> verify_match{};
    std::atomic<u64> verify_mismatch{};
};
Counters g_counters;

/// Buffer kinds the lane may leave to the bind as before: they are not guest memory, so no cache keyed on guest
/// addresses would have saved their binding. Every other special buffer keeps the caches (GDS, the page table, the fault
/// buffer, LDS, the BDA block table): not known to be covered, so not eligible.
bool SkippableSpecial(Shader::BufferType type) {
    return type == Shader::BufferType::Flatbuf || type == Shader::BufferType::ClipPlanes ||
           type == Shader::BufferType::LodFeedback;
}

/// The pipeline's part of the answer: its profile and stages, fixed once the pipeline is made (cached by Eligible).
/// The switch terms (Buf, Sync, Barrier) are not here: Refuse may lower them at run time.
bool ComputePipelineEligible(const Vulkan::Pipeline& pipeline) {
    const Shader::Profile& profile = pipeline.GetProfile();
    if (profile.gcn_buf == 0) {
        return false;
    }
    // The image path of a heap stage: the T# table answers and the per-op Resolve is skipped.
    // sotc: heap_images == 1: with the heap GPU verify (2) the classic bind still runs for the stage.
    const bool table_images = profile.gcn_tex == 1 && profile.heap_images == 1 && Tex::SkipResolve();
    u32 heap_vertex = 0; // sotc: heap stages by kind, as BindResources picks them (two of one kind: the op is dropped)
    u32 heap_other = 0;
    for (const Shader::Info* stage : pipeline.GetStages()) {
        if (!stage) {
            continue;
        }
        // Pointers the shader dereferences itself, descriptor arrays from tables and fmasks keep the caches.
        if (stage->uses_dma || !stage->sharp_tables.empty() || !stage->fmasks.empty()) {
            return false;
        }
        if (Shader::Backend::SPIRV::UsesHeapImages(*stage, profile)) {
            (stage->sw_stage == Shader::SwStage::Vertex ? heap_vertex : heap_other) += 1;
            if (heap_vertex > 1 || heap_other > 1) {
                return false;
            }
        }
        for (u32 i = 0; i < stage->buffers.size(); ++i) {
            const Shader::BufferResource& desc = stage->buffers[i];
            if (desc.IsSpecial()) {
                if (!SkippableSpecial(desc.buffer_type)) {
                    return false;
                }
                continue;
            }
            if (!Shader::ShaderAddrBufferAt(profile, *stage, i)) {
                return false;
            }
        }
        if (!stage->images.empty() || !stage->samplers.empty() || stage->has_storage_images) {
            const bool heap = Shader::Backend::SPIRV::UsesHeapImages(*stage, profile);
            if (!heap || !table_images || !Shader::Gcn::TableStage(*stage)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool On() {
    return Mode(Stage::Commit) != 0;
}

bool Verify() {
    return Mode(Stage::Commit) == 2;
}

bool Eligible(const Vulkan::Pipeline& pipeline) {
    if (!On()) {
        return false;
    }
    // The global terms are read on every call: Buf, Sync and Barrier can be lowered at run time (a new pipeline's
    // ProfileValue refuses Buf/Tex), and a cached true would outlive them. Each is one relaxed load after the first read.
    if (!Buf::On() || !Sync::SkipOpSync() || !Barrier::On()) {
        return false;
    }
    // The pipeline's own part never changes, so it is cached on the pipeline (-1 unknown; draw thread only).
    const s8 cached = pipeline.sotc_gcn_commit.load(std::memory_order_relaxed);
    if (cached >= 0) {
        return cached != 0;
    }
    const bool eligible = ComputePipelineEligible(pipeline);
    pipeline.sotc_gcn_commit.store(eligible ? 1 : 0, std::memory_order_relaxed);
    return eligible;
}

void NoteOp(bool eligible, bool is_dispatch) {
    g_counters.ops.fetch_add(1, std::memory_order_relaxed);
    if (eligible) {
        g_counters.eligible_ops.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_dispatch) {
        g_counters.dispatches.fetch_add(1, std::memory_order_relaxed);
        if (eligible) {
            g_counters.eligible_dispatches.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_counters.draws.fetch_add(1, std::memory_order_relaxed);
        if (eligible) {
            g_counters.eligible_draws.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void NoteBypass() {
    g_counters.bypass.fetch_add(1, std::memory_order_relaxed);
}

void NoteDispatchPrepared(bool taken) {
    if (taken) {
        g_counters.dispatch_taken.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_counters.dispatch_not.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteDispatchVerify(bool match) {
    if (match) {
        g_counters.verify_match.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_counters.verify_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    if (!On()) {
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const u64 ops = take(g_counters.ops);
    const u64 eligible_ops = take(g_counters.eligible_ops);
    const u64 draws = take(g_counters.draws);
    const u64 eligible_draws = take(g_counters.eligible_draws);
    const u64 dispatches = take(g_counters.dispatches);
    const u64 eligible_dispatches = take(g_counters.eligible_dispatches);
    const u64 bypass = take(g_counters.bypass);
    const u64 taken = take(g_counters.dispatch_taken);
    const u64 not_taken = take(g_counters.dispatch_not);
    const u64 match = take(g_counters.verify_match);
    const u64 mismatch = take(g_counters.verify_mismatch);

    const double secs = seconds > 0.0 ? seconds : 1.0;
    const auto pct = [](u64 part, u64 whole) {
        return whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
    };
    std::snprintf(out, size,
                  "SOTC GCN COMMIT: ops %.0f/s eligible %.1f%% (draws %.1f%%, dispatches %.1f%%), bypass %.0f/s, "
                  "dispatches prepared %.1f%% (%llu taken, %llu not), verify match %llu mismatch %llu",
                  static_cast<double>(ops) / secs, pct(eligible_ops, ops), pct(eligible_draws, draws),
                  pct(eligible_dispatches, dispatches), static_cast<double>(bypass) / secs,
                  pct(taken, taken + not_taken), static_cast<unsigned long long>(taken),
                  static_cast<unsigned long long>(not_taken), static_cast<unsigned long long>(match),
                  static_cast<unsigned long long>(mismatch));
}

} // namespace SotcGcn::Commit
