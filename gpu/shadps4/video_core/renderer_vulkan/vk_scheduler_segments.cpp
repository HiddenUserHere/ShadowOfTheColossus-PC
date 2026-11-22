// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: W5 pass recorder (SOTC_PASS_RECORDER, N2-94 plan): segments and the submit thread of
// Vulkan::Scheduler. Owner: H10.
//
// Coverage (N2-94, H10, fixer round 3):
// item                                                      | done? | evidence
// ----------------------------------------------------------|-------|-------------------------------
// EndSegment: end upload + primary on the producer, push    | yes   | EndSegment below (SPEC order)
// EndSegment: new segment begun, state reset                | yes   | EndSegment below
// StartSubmitThread ("shadPS4:GpuSubmit")                   | yes   | StartSubmitThread below
// SubmitThreadLoop: untimed wait, stop aware, drains queue  | yes   | SubmitThreadLoop below
// Deferred segments: func recorded then ended by the thread | yes   | SubmitThreadLoop
// Non-deferred segments: ended once, by the producer        | yes   | EndSegment, EndSession (vk_scheduler.cpp)
// QueueSubmit with all cmdbufs in order, tick published     | yes   | SubmitThreadLoop
// after_submit run after the submit                         | yes   | SubmitThreadLoop
// QueueSegmentBatch: push + notify, bounded queue           | yes   | QueueSegmentBatch
// BeginSegmentBuffer from command_pool on the producer      | yes   | BeginSegmentBuffer
// Deferred segment: second producer EndSegment (SPEC)       | deviation | would end it before its func runs
//                                                           |       | (invalid: see RecordDeferred); needs acceptance
// Sync (SPEC lists it): no Scheduler::Sync in the tree      | blocked | needs a header decl (vk_scheduler.h, not owned)
//                                                           |       | and orchestrator confirmation of name/semantics
// QueueSubmit forward decl (line 33): external linkage     | nit   | move the decl to vk_scheduler.h (core_request)
// Retire (recorder path) in pass mode                       | n/a   | no fake handles, nothing to retire
// PopPendingOperations ordering vs GPU submit               | note  | runs right after queueing (see SubmitExecution)

#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"

namespace Vulkan {

// Defined in vk_scheduler.cpp: the queue submit of SubmitExecution.
u64 QueueSubmit(const Instance& instance, const SubmitInfo& info,
                std::span<const vk::CommandBuffer> cmd_buffers);

namespace {
// sotc: producer backpressure: at most this many submits wait for the submit thread.
constexpr std::size_t kMaxQueuedBatches = 16;
} // namespace

void Scheduler::EndSegment() {
    if (!pass_recording || sessions.empty()) {
        return;
    }
    // sotc: W5 (N2-94): a segment never holds a rendering scope, the next one opens its own.
    EndRendering();
    auto& session = sessions.back();
    // sotc: W5 (N2-94): the producer ends what it closes (SPEC order): the open upload (its commands
    // were queued in stream order by UploadCommandBuffer, and the next upload is a new buffer), then
    // the primary, which goes to open_segments. A segment that carries a deferred func is the one
    // exception: RecordDeferred pushes it unended, because the submit thread records the func into
    // it and ends it (SubmitThreadLoop). Each buffer is ended exactly once.
    if (session.upload) {
        Check(session.upload.end());
        session.upload = vk::CommandBuffer{};
    }
    Check(session.primary.end());
    open_segments.push_back(Segment{session.primary, {}});
    BeginSegmentBuffer();
    dynamic_state.Invalidate();
    is_rendering = false;
    render_state = RenderState{};
    render_state_token = 0;
}

void Scheduler::StartSubmitThread() {
    submit_thread = std::jthread([this](std::stop_token stoken) { SubmitThreadLoop(stoken); });
}

void Scheduler::SubmitThreadLoop(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSubmit");
    while (true) {
        SegmentBatch batch;
        {
            std::unique_lock lk{submit_queue_mutex};
            // sotc: untimed wait; a stop request still lets the queued batches go out.
            submit_queue_cv.wait(lk, stoken, [this] { return !submit_queue.empty(); });
            if (submit_queue.empty()) {
                break;
            }
            batch = std::move(submit_queue.front());
            submit_queue.pop_front();
        }
        // The producer waits for room in the queue on the same condition variable.
        submit_queue_cv.notify_all();

        std::vector<vk::CommandBuffer> cmd_buffers;
        cmd_buffers.reserve(batch.segments.size());
        for (Segment& segment : batch.segments) {
            if (segment.deferred) {
                // sotc: the func records into the segment right before the submit (N2-94 W5), and
                // this thread is the only one to end it: the producer left it open.
                segment.deferred(vk::CommandBuffer{segment.cmdbuf});
                Check(segment.cmdbuf.end());
            }
            // Other segments were ended by the producer (EndSegment / EndSession).
            cmd_buffers.push_back(segment.cmdbuf);
        }

        const u64 queue_us = QueueSubmit(instance, batch.info, cmd_buffers);
        // sotc: N2-94 H10 fix: gate on the compile-time constant, not the plain bool `tickwatch`
        // (StartTickWatch writes it on another thread while this thread is running: a data race).
        if (SotcStats::kTickWatch) {
            SotcStats::PushTick(2, batch.tick, queue_us);
        }
        recorded_submit_tick.store(batch.tick, std::memory_order_release);
        recorded_submit_tick.notify_all();
        if (batch.after_submit) {
            batch.after_submit();
        }
    }
}

void Scheduler::QueueSegmentBatch(SegmentBatch&& batch) {
    {
        std::unique_lock lk{submit_queue_mutex};
        submit_queue_cv.wait(lk, [this] { return submit_queue.size() < kMaxQueuedBatches; });
        submit_queue.push_back(std::move(batch));
    }
    submit_queue_cv.notify_all();
}

void Scheduler::BeginSegmentBuffer() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    const vk::CommandBuffer cmdbuf = command_pool.Commit();
    Check(cmdbuf.begin(begin_info));
    sessions.back().primary = cmdbuf;
}

} // namespace Vulkan
