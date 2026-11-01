// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// sotc: N5 frame graph (SOTC_N5_GRAPH=1 observe, =2 verify; both need SOTC_NATIVE on).
//
// The draw thread already knows, per op, its render targets (back registers) and its guest
// memory footprint (AmdGpu::OpFootprint, filled by the prep threads, see liverpool.cpp N5Begin).
// This layer groups the ops of a frame into passes and predicts where a barrier is needed from
// the footprints alone:
//  - a pass is a maximal run of graphics ops with the same render-target identity, or of
//    compute ops (Class::Graphics / Class::Compute); command-processor ops (Class::Other) end it;
//  - a hazard group is a run of ops whose footprints do not conflict (AmdGpu::Conflicts) with
//    the union of the group. A barrier is predicted before the first op that conflicts with the
//    group, before every op that follows an unknown footprint, and when the group grows past a
//    bound. Hazard groups cross pass boundaries: a render target read by the next pass is a
//    conflict like any other range.
//
// Observe only. Nothing here changes what is recorded: the old per-op barrier path still runs,
// and the verify counters compare its barrier events (Runtime::FlushBarriers and the rasterizer
// barrier sites call NoteBarrier) with the prediction, op by op. "missed" = an op that emitted a
// barrier the footprints did not predict: the graph would be incomplete for that op.
//
// Threads: Record, Begin/End and the draw-thread state are used only by the draw thread (the
// thread that runs ExecuteBackOp, which also runs the express lane). NoteBarrier counts on the
// thread that records the barrier; BarrierEvents reads the calling thread's count. Report may be
// called from any thread.

#include <string>

#include "common/types.h"
#include "video_core/amdgpu/op_footprint.h"

namespace Vulkan::FrameGraph {

enum class Class : u8 {
    Graphics, ///< draw (direct or indirect) with a pipeline
    Compute,  ///< dispatch (direct or indirect)
    Other,    ///< command-processor op (WRITE_DATA, EOP, DMA, ...): unknown footprint
};

/// One op, opened by Liverpool::N5Begin and closed by Record (through N5End).
struct Sample {
    bool active{};
    Class cls{Class::Other};
    bool known{};         ///< footprint known (Graphics / Compute only)
    u64 target_key{};     ///< Graphics: render-target identity (0 for compute and other)
    AmdGpu::OpFootprint fp{};
    u64 barrier0{};       ///< BarrierEvents() when the op began
};

/// SOTC_NATIVE on and SOTC_N5_GRAPH=1 or =2 (read once).
bool Enabled();
/// SOTC_N5_GRAPH=2: also logs the first ops that emitted a barrier the prediction missed.
bool Verify();

/// Counts one barrier recorded on the calling thread. Call it right before every
/// vkCmdPipelineBarrier / pipelineBarrier2 that a draw or dispatch can reach.
void NoteBarrier(u32 count = 1) noexcept;
/// Barrier events recorded so far on the calling thread.
u64 BarrierEvents() noexcept;

/// Owner time spent computing an op's footprint (Liverpool::N5Begin), in ns.
void AddOwnerNs(u64 ns) noexcept;

/// Draw thread: closes one op. Updates the pass and hazard state and the window counters.
void Record(Sample& s);

/// Appends one 5 s report line (the caller's window). Resets the window counters.
void Report(std::string& out, double seconds);

} // namespace Vulkan::FrameGraph
