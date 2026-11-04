// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_BARRIER (S5, decision D11). Static barriers for the GCN-native path: while every texture-cache image
// is GENERAL (SOTC_LAYOUT_UNIFIED active), a transition between shader-access layouts is dropped (image.cpp calls
// DropTransition), and the ordering it gave is done by one global memory barrier per writer op (recorded lazily
// before the next op, outside render passes) and one at each render pass end.
//
// Modes (gcn_native.h, Stage::Barrier): 0 off, nothing here runs. 1 on. 2 verify: DropTransition answers false (the
// old barriers are emitted) and counts what it would drop; the global barriers are counted but not recorded.
// Refused (mode 0 for the run) when the unified layouts are not active.
//
// Threading: the draw thread (op barriers, render pass ends) and the texture cache, under its lock, call these. The
// pending flag and the counters are atomics: the flag is read and written by the draw thread only.
#include <atomic>
#include <cstdio>

#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/gcn_native.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcGcn::Barrier {

namespace {

// The global barrier, used both before an op (after writers) and at a render pass end. Its sources are every write a
// shader, an attachment, a copy or an indirect argument can leave; its destinations are every read and write that
// follows: indirect arguments, index and vertex fetches, shader reads and writes, attachments and transfers.
constexpr vk::PipelineStageFlags2 kSrcStages =
    vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests |
    vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eColorAttachmentOutput |
    vk::PipelineStageFlagBits2::eTransfer;
constexpr vk::AccessFlags2 kSrcAccess =
    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
    vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eTransferWrite;
constexpr vk::PipelineStageFlags2 kDstStages =
    vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexInput |
    vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests |
    vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eColorAttachmentOutput |
    vk::PipelineStageFlagBits2::eTransfer;
constexpr vk::AccessFlags2 kDstAccess =
    vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eIndexRead |
    vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eShaderRead |
    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferRead |
    vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eColorAttachmentRead |
    vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentRead |
    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;

// sotc: set once, on the first On() call (the unified layouts are known by then).
std::atomic<bool> g_checked{false};
// sotc: a writer op ran since the last global barrier (AfterOp), or the pass-end barrier is still to come.
std::atomic<bool> g_pending{false};

std::atomic<u64> c_dropped{};       // mode 1: transitions dropped
std::atomic<u64> c_would_drop{};    // mode 2: transitions the rule would drop
std::atomic<u64> c_after_writers{}; // global barriers before an op (after writers)
std::atomic<u64> c_pass_ends{};     // global barriers at render pass ends
std::atomic<u64> c_deferred{};      // ops that wanted the barrier inside a render pass and waited for its end

// Records the global barrier on the scheduler's current command buffer (outside a render pass).
void Record(Vulkan::Scheduler& scheduler) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = kSrcStages,
        .srcAccessMask = kSrcAccess,
        .dstStageMask = kDstStages,
        .dstAccessMask = kDstAccess,
    };
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

} // namespace

bool On() {
    if (Mode(Stage::Barrier) == 0) {
        return false;
    }
    if (!g_checked.load(std::memory_order_acquire)) {
        if (!SotcArch::Layouts::Active()) {
            Refuse(Stage::Barrier, "layouts unified not active");
        }
        g_checked.store(true, std::memory_order_release);
    }
    return Mode(Stage::Barrier) != 0;
}

bool Verify() {
    return Mode(Stage::Barrier) == 2;
}

bool DropTransition(vk::ImageLayout old_layout, vk::ImageLayout new_layout) {
    if (!On() || !SotcArch::Layouts::Active()) {
        return false;
    }
    // Pick maps every layout to GENERAL except UNDEFINED (first use) and the present layouts, which keep their barrier.
    if (SotcArch::Layouts::Pick(old_layout) != vk::ImageLayout::eGeneral ||
        SotcArch::Layouts::Pick(new_layout) != vk::ImageLayout::eGeneral) {
        return false;
    }
    if (Verify()) {
        c_would_drop.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    c_dropped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void AfterOp(bool writes_memory) {
    if (writes_memory && On()) {
        g_pending.store(true, std::memory_order_relaxed);
    }
}

void BeforeOp(Vulkan::Scheduler& scheduler, bool in_render_pass) {
    if (!On() || !g_pending.load(std::memory_order_relaxed)) {
        return;
    }
    // A pass is open (the previous op's draw, or the caller's flag): the barrier cannot be recorded inside it, and the
    // op that follows would run before it. Ends the pass; Scheduler::EndRendering -> OnPassBoundary records the barrier
    // and clears the flag. Callers call this before their BeginRendering. Verify keeps the pass (counts only).
    if (in_render_pass || scheduler.IsRendering()) {
        c_deferred.fetch_add(1, std::memory_order_relaxed);
        if (!Verify()) {
            scheduler.EndRendering();
        }
        return;
    }
    g_pending.store(false, std::memory_order_relaxed);
    c_after_writers.fetch_add(1, std::memory_order_relaxed);
    if (!Verify()) {
        Record(scheduler);
    }
}

void OnPassBoundary(Vulkan::Scheduler& scheduler) {
    if (!On()) {
        return;
    }
    // Called once the scheduler has ended rendering (outside the pass): covers the attachment writes of the pass and
    // every shader write since the last barrier.
    g_pending.store(false, std::memory_order_relaxed);
    c_pass_ends.fetch_add(1, std::memory_order_relaxed);
    if (!Verify()) {
        Record(scheduler);
    }
}

void Report(char* out, u32 size, double seconds) {
    out[0] = '\0';
    if (Mode(Stage::Barrier) == 0) {
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const double per_s = seconds > 0.0 ? 1.0 / seconds : 0.0;
    const u64 dropped = take(c_dropped);
    const u64 would = take(c_would_drop);
    const u64 after = take(c_after_writers);
    const u64 pass = take(c_pass_ends);
    const u64 deferred = take(c_deferred);
    std::snprintf(out, size,
                  "SOTC GCN BARRIER: transitions dropped %.0f/s (verify would-drop %.0f/s), global barriers %.1f/s "
                  "(after writers %.1f/s, pass ends %.1f/s), deferred in pass %.1f/s",
                  static_cast<double>(dropped) * per_s, static_cast<double>(would) * per_s,
                  static_cast<double>(after + pass) * per_s, static_cast<double>(after) * per_s,
                  static_cast<double>(pass) * per_s, static_cast<double>(deferred) * per_s);
}

} // namespace SotcGcn::Barrier
