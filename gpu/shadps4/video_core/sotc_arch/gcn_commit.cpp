// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_COMMIT (S6, decisions D13-D15). The commit lane: an op whose pipeline is commit-eligible skips the bind
// caches and binds directly (vk_rasterizer.cpp, BindResources), and dispatches are prepared on the GpuPrep threads.
// This file holds the switch's helpers, the eligibility of a pipeline and the counters. gcn_native.h, namespace Commit.
//
// Modes: 0 off. 1 on (eligible ops bypass the caches). 2 verify (the caches run as before; eligible ops are counted).
// sotc: also SOTC_GCN_SKIP (plan step 4, gcn_native.h): its switch, pipeline eligibility and counters (end of the file).
#include <atomic>
#include <chrono>
#include <cstdio>

#include "common/logging/log.h"
#include "common/types.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/gcn_rules.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/sotc_arch/gcn_native.h"
#include "video_core/sotc_arch/shader_addr.h" // sotc: SOTC_GCN_SKIP (ShaderAddr::Verify)
#include "video_core/sotc_arch/sotc_arch.h"   // sotc: SOTC_GCN_SKIP (SwitchMode)

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

// ---------------------------------------------------------------------------------------------------------------
// sotc: SOTC_GCN_SKIP (GCN-NATIVE-PLAN step 4). The switch, the eligibility of a pipeline and the counters; the skip
// path itself is in vk_rasterizer.cpp (BindResources, BindBuffersSkip, BindBuffersSkipVerify).

namespace {

/// Counters of the 5 s skip report (relaxed; read and reset by SkipReport).
struct SkipCounters {
    std::atomic<u64> ops{};
    std::atomic<u64> refused[static_cast<u32>(SkipReason::Count)]{};
    std::atomic<u64> done_buffers{};   // skipped (or, mode 2, would be): buffers only, BindTextures kept
    std::atomic<u64> done_images{};    // skipped: buffers and images
    std::atomic<u64> formatted_fast{}; // formatted reads answered by the placed-page test
    std::atomic<u64> formatted_slow{}; // formatted reads that ran GcnTouch
    std::atomic<u64> bind_ticks[3]{};  // BindResources TSC ticks: refused, skipped, eligible + full bind (mode 2)
    std::atomic<u64> bind_ops[3]{};
    std::atomic<u64> verify_stages{};
    std::atomic<u64> verify_mismatch_stages{};
    std::atomic<u64> verify_category[SkipMmCount]{};
};
SkipCounters g_skip;
std::atomic<u32> g_skip_logged{};

constexpr const char* kSkipMismatchNames[SkipMmCount] = {"push", "packet", "ranges", "written", "descriptors",
                                                         "bindings"};

/// The pipeline's part of the skip answer (cached by SkipEligible): its profile and stages, fixed once it is made.
/// Images never refuse: a stage whose images are not table-resolved keeps BindTextures (buffer-only skip).
SkipReason ComputePipelineSkip(const Vulkan::Pipeline& pipeline) {
    const Shader::Profile& profile = pipeline.GetProfile();
    // Table-mode packet pipelines only: the shader reads the page table through the packet words (BindResources).
    if (profile.gcn_buf == 0 || !pipeline.HasPacketRange()) {
        return SkipReason::Profile;
    }
    for (const Shader::Info* stage : pipeline.GetStages()) {
        if (!stage) {
            continue;
        }
        // As the commit lane's whitelist: pointers the shader dereferences, descriptor arrays from tables and fmasks.
        if (stage->uses_dma || !stage->sharp_tables.empty() || !stage->fmasks.empty()) {
            return SkipReason::Dma;
        }
        for (u32 i = 0; i < stage->buffers.size(); ++i) {
            const Shader::BufferResource& desc = stage->buffers[i];
            if (desc.IsSpecial()) {
                // BindBuffersSkip binds these three as BindBuffers does; every other special keeps the full bind.
                if (!SkippableSpecial(desc.buffer_type)) {
                    return SkipReason::Special;
                }
                continue;
            }
            if (!Shader::ShaderAddrBufferAt(profile, *stage, i)) {
                return SkipReason::Unaddressed;
            }
        }
    }
    return SkipReason::None;
}

} // namespace

int SkipMode() {
    static const int mode = [] {
        const int m = SotcArch::SwitchMode("SOTC_GCN_SKIP");
        if (m == 0) {
            return 0;
        }
        // The skip path reads every guest buffer by address (S2) and leaves freshness to the submit batch (S4).
        if (Mode(Stage::Buf) == 0 || Mode(Stage::Sync) == 0) {
            LOG_WARNING(Render, "SOTC GCN: skip refused: needs SOTC_GCN_BUF and SOTC_GCN_SYNC");
            return 0;
        }
        LOG_INFO(Render, "SOTC GCN: skip={} (eligible ops skip BindBuffers, and BindHeapImages when table-resolved)",
                 m);
        return m;
    }();
    return mode;
}

SkipReason SkipEligible(const Vulkan::Pipeline& pipeline) {
    // Global terms on every call: Buf and Sync can be refused after the first use (a cached answer would outlive them).
    // ProfileValue is 0 unless SOTC_SHADER_ADDR is in table mode with SOTC_GENERIC_BUFFERS=2.
    if (Buf::ProfileValue() == 0 || !Sync::On() || SotcArch::ShaderAddr::Verify()) {
        return SkipReason::Global;
    }
    const s8 cached = pipeline.sotc_gcn_skip.load(std::memory_order_relaxed);
    if (cached >= 0) {
        return static_cast<SkipReason>(cached);
    }
    const SkipReason reason = ComputePipelineSkip(pipeline);
    pipeline.sotc_gcn_skip.store(static_cast<s8>(reason), std::memory_order_relaxed);
    return reason;
}

void NoteSkipDecision(SkipReason reason) {
    g_skip.ops.fetch_add(1, std::memory_order_relaxed);
    if (reason != SkipReason::None) {
        g_skip.refused[static_cast<u32>(reason)].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteSkipDone(bool with_images) {
    (with_images ? g_skip.done_images : g_skip.done_buffers).fetch_add(1, std::memory_order_relaxed);
}

void NoteSkipFormatted(bool fast) {
    (fast ? g_skip.formatted_fast : g_skip.formatted_slow).fetch_add(1, std::memory_order_relaxed);
}

void NoteSkipBindTime(u32 kind, u64 ticks) {
    if (kind < 3) {
        g_skip.bind_ticks[kind].fetch_add(ticks, std::memory_order_relaxed);
        g_skip.bind_ops[kind].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteSkipVerify(u32 mask, u64 pgm_hash, u32 stage_index) {
    g_skip.verify_stages.fetch_add(1, std::memory_order_relaxed);
    if (mask == 0) {
        return;
    }
    g_skip.verify_mismatch_stages.fetch_add(1, std::memory_order_relaxed);
    char names[96];
    names[0] = '\0';
    u32 used = 0;
    for (u32 c = 0; c < SkipMmCount; ++c) {
        if (((mask >> c) & 1u) == 0) {
            continue;
        }
        g_skip.verify_category[c].fetch_add(1, std::memory_order_relaxed);
        const int n =
            std::snprintf(names + used, sizeof names - used, "%s%s", used != 0 ? "," : "", kSkipMismatchNames[c]);
        if (n > 0 && used + static_cast<u32>(n) < sizeof names) {
            used += static_cast<u32>(n);
        }
    }
    if (g_skip_logged.fetch_add(1, std::memory_order_relaxed) < 32) {
        std::fprintf(stderr, "SOTC GCN SKIP verify mismatch: shader %#llx stage %u: %s\n",
                     static_cast<unsigned long long>(pgm_hash), stage_index, names);
    }
}

void SkipReport(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    const int mode = SkipMode();
    if (mode == 0) {
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const u64 ops = take(g_skip.ops);
    u64 refused[static_cast<u32>(SkipReason::Count)];
    u64 refused_all = 0;
    for (u32 r = 0; r < static_cast<u32>(SkipReason::Count); ++r) {
        refused[r] = take(g_skip.refused[r]);
        refused_all += refused[r];
    }
    const u64 done_buffers = take(g_skip.done_buffers);
    const u64 done_images = take(g_skip.done_images);
    const u64 fmt_fast = take(g_skip.formatted_fast);
    const u64 fmt_slow = take(g_skip.formatted_slow);
    u64 ticks[3];
    u64 bops[3];
    for (u32 k = 0; k < 3; ++k) {
        ticks[k] = take(g_skip.bind_ticks[k]);
        bops[k] = take(g_skip.bind_ops[k]);
    }
    const u64 vstages = take(g_skip.verify_stages);
    const u64 vmis = take(g_skip.verify_mismatch_stages);
    u64 vcat[SkipMmCount];
    for (u32 c = 0; c < SkipMmCount; ++c) {
        vcat[c] = take(g_skip.verify_category[c]);
    }
    // TSC ticks per ms, calibrated between two reports against the steady clock (the first report prints 0 times).
    static u64 last_tsc = 0;
    static std::chrono::steady_clock::time_point last_time{};
    const u64 now_tsc = __builtin_ia32_rdtsc();
    const auto now_time = std::chrono::steady_clock::now();
    double tsc_per_ms = 0.0;
    if (last_tsc != 0) {
        const double ms = std::chrono::duration<double, std::milli>(now_time - last_time).count();
        if (ms > 0.0) {
            tsc_per_ms = static_cast<double>(now_tsc - last_tsc) / ms;
        }
    }
    last_tsc = now_tsc;
    last_time = now_time;
    const double secs = seconds > 0.0 ? seconds : 1.0;
    // Draw-thread ms per second spent in BindResources, and us per op, of one kind.
    const auto ms_per_s = [&](u32 k) {
        return tsc_per_ms > 0.0 ? static_cast<double>(ticks[k]) / tsc_per_ms / secs : 0.0;
    };
    const auto us_per_op = [&](u32 k) {
        return tsc_per_ms > 0.0 && bops[k] != 0
                   ? 1000.0 * static_cast<double>(ticks[k]) / tsc_per_ms / static_cast<double>(bops[k])
                   : 0.0;
    };
    const auto pct = [&](u64 part) {
        return ops != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(ops) : 0.0;
    };
    const u32 eligible_kind = mode == 1 ? 1u : 2u;
    const int n = std::snprintf(
        out, size,
        "SOTC GCN SKIP: mode %d, ops %.0f/s, eligible %.1f%% (%s: buffers only %.0f/s, buffers+images %.0f/s), "
        "refused global %.1f%% profile %.1f%% dma %.1f%% special %.1f%% unaddressed %.1f%%; formatted reads fast %llu "
        "slow %llu; BindResources ms/s: refused ops %.2f (%.2f us/op), %s %.2f (%.2f us/op)",
        mode, static_cast<double>(ops) / secs, pct(ops - refused_all), mode == 1 ? "skipped" : "would skip",
        static_cast<double>(done_buffers) / secs, static_cast<double>(done_images) / secs,
        pct(refused[static_cast<u32>(SkipReason::Global)]), pct(refused[static_cast<u32>(SkipReason::Profile)]),
        pct(refused[static_cast<u32>(SkipReason::Dma)]), pct(refused[static_cast<u32>(SkipReason::Special)]),
        pct(refused[static_cast<u32>(SkipReason::Unaddressed)]), static_cast<unsigned long long>(fmt_fast),
        static_cast<unsigned long long>(fmt_slow), ms_per_s(0), us_per_op(0),
        mode == 1 ? "skipped ops" : "eligible ops (full bind)", ms_per_s(eligible_kind), us_per_op(eligible_kind));
    if (mode == 2 && n >= 0 && static_cast<u32>(n) < size) {
        std::snprintf(out + n, size - static_cast<u32>(n),
                      "; verify stages %llu mismatch %llu (push %llu packet %llu ranges %llu written %llu "
                      "descriptors %llu bindings %llu)",
                      static_cast<unsigned long long>(vstages), static_cast<unsigned long long>(vmis),
                      static_cast<unsigned long long>(vcat[0]), static_cast<unsigned long long>(vcat[1]),
                      static_cast<unsigned long long>(vcat[2]), static_cast<unsigned long long>(vcat[3]),
                      static_cast<unsigned long long>(vcat[4]), static_cast<unsigned long long>(vcat[5]));
    }
}

} // namespace SotcGcn::Commit
