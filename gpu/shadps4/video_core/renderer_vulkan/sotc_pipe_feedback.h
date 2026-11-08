// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: pipeline compile measurement (see sotc_pipe_feedback.cpp for the report).
//   SOTC_PIPE_FEEDBACK=1       chains VkPipelineCreationFeedbackCreateInfo into every pipeline build and
//                              reports the driver time per kind, the wall time of the create calls and the
//                              20 slowest pipelines (printed at the first report with entries, then every
//                              30 s). Default off.
//   SOTC_PIPE_FEEDBACK_OUT=<file>  one TSV line per created pipeline: kind, debug string (the shader
//                              hashes), driver ms, wall ms, thread name, cache hit (0/1). Written under a
//                              mutex, flushed about once a second. Implies the measurement above (the
//                              chain is built), but the summary report stays on SOTC_PIPE_FEEDBACK only.
//                              Default off.
//   SOTC_PIPE_COMPILE_CHECK=1  logs each pipeline the recorder waited for, or that was built on a thread
//                              other than a pipeline worker, once per pipeline. When the device enables
//                              pipelineCreationCacheControl (SetCompileControlEnabled), every build first
//                              probes the driver cache with VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED.
//                              Default off.
// With both off, a pipeline build costs one static bool test per switch.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::SotcPipeFeedback {

enum class Kind : u32 {
    Compute,
    Graphics,
    GplLibrary,
    GplLink,
    Count,
};

/// Largest stage count a feedback struct can hold (graphics pipelines have at most 5 stages).
constexpr u32 MaxStages = 8;

/// sotc: SOTC_PIPE_FEEDBACK. Empty or "0" is off.
inline bool FeedbackOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PIPE_FEEDBACK");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

/// sotc: SOTC_PIPE_FEEDBACK_OUT=<file> (the TSV path). Empty or unset is off. Read once.
inline const char* TsvPath() {
    static const char* const path = [] {
        const char* v = std::getenv("SOTC_PIPE_FEEDBACK_OUT");
        return (v != nullptr && v[0] != '\0') ? v : nullptr;
    }();
    return path;
}

inline bool TsvOn() {
    return TsvPath() != nullptr;
}

/// sotc: SOTC_PIPE_COMPILE_CHECK. Empty or "0" is off.
inline bool CompileCheckOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PIPE_COMPILE_CHECK");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

/// sotc: set by the device setup when pipelineCreationCacheControl is enabled on the device. The
/// FAIL_ON_PIPELINE_COMPILE_REQUIRED flag is invalid without it, so the probe stays off until then.
inline std::atomic<bool> g_compile_control{false};

/// sotc: the driver-cache probe runs only with SOTC_PIPE_COMPILE_CHECK and the device feature.
inline bool ProbeEnabled() {
    return CompileCheckOn() && g_compile_control.load(std::memory_order_relaxed);
}

/// sotc: driver duration (ns) of the 20th slowest pipeline once the list is full, else 0. Read
/// without the lock, so a pipeline only takes the lock when it can enter the list.
inline std::atomic<u64> g_top_threshold_ns{0};

inline bool RanksTop(u64 driver_ns) noexcept {
    return driver_ns > g_top_threshold_ns.load(std::memory_order_relaxed);
}

// Out-of-line parts (sotc_pipe_feedback.cpp).
void Record(Kind kind, u64 driver_ns, u64 wall_ns, bool hit) noexcept;
void CountFailed(Kind kind) noexcept;
void AddTop(Kind kind, u64 driver_ns, u64 wall_ns, bool hit, std::string name);
void AppendTsv(Kind kind, const std::string& name, u64 driver_ns, u64 wall_ns, bool hit);
void NoteProbe(vk::Result result) noexcept;
bool OnPipelineWorker();
void LogOnce(Kind kind, const char* what, std::atomic<bool>* once, const std::string& name);

/// One create call. Chains the feedback struct in front of the caller's pNext chain when
/// SOTC_PIPE_FEEDBACK is on; otherwise Next() is the caller's chain and nothing is timed.
class Call {
public:
    Call(Kind kind_, u32 stage_count, const void* next_) noexcept
        : kind{kind_}, next{next_} {
        on = (FeedbackOn() || TsvOn()) && stage_count <= MaxStages;
        if (!on) {
            return;
        }
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
        info.pNext = next_;
        info.pPipelineCreationFeedback = &total;
        info.pipelineStageCreationFeedbackCount = stage_count;
        info.pPipelineStageCreationFeedbacks = stage_fb.data();
        start = std::chrono::steady_clock::now();
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    /// The pNext of the create info: the feedback struct when on, else the caller's chain.
    [[nodiscard]] const void* Next() noexcept {
        return on ? static_cast<const void*>(&info) : next;
    }

    /// Records the call after it returned. name() gives the pipeline's debug string and is only
    /// called when the pipeline enters the top list.
    template <typename Name>
    void Done(vk::Result result, Name&& name) {
        if (!on) {
            return;
        }
        const auto wall = std::chrono::steady_clock::now() - start;
        const u64 wall_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count());
        // A probe miss is counted by NoteProbe, not as a failed build.
        if (result == vk::Result::ePipelineCompileRequiredEXT) {
            return;
        }
        if (result != vk::Result::eSuccess) {
            CountFailed(kind);
            return;
        }
        const bool valid = (total.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0;
        const u64 driver_ns = valid ? total.duration : 0;
        const bool hit =
            (total.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
        Record(kind, driver_ns, wall_ns, hit);
        // The name is built only when a consumer needs it (TSV line, or the top list).
        const bool top = FeedbackOn() && RanksTop(driver_ns);
        if (top || TsvOn()) {
            std::string text(name());
            if (TsvOn()) {
                AppendTsv(kind, text, driver_ns, wall_ns, hit);
            }
            if (top) {
                AddTop(kind, driver_ns, wall_ns, hit, std::move(text));
            }
        }
    }

private:
    Kind kind;
    const void* next;
    bool on{};
    std::chrono::steady_clock::time_point start{};
    VkPipelineCreationFeedbackCreateInfo info{};
    VkPipelineCreationFeedback total{};
    std::array<VkPipelineCreationFeedback, MaxStages> stage_fb{};
};

/// Builds one pipeline. With the probe on (probe_ok, not a heap pipeline), first asks the driver
/// with FAIL_ON_PIPELINE_COMPILE_REQUIRED: a success is the pipeline (a driver cache hit) and is
/// returned; a compile-required result is a driver cache miss, and the normal build follows.
/// make(next, fail_on_compile_required) runs one create call and returns {result, handle}.
/// sotc: SOTC_PLAY_QUIET: when the last pipeline (library, link, full or compute) was created on any thread, steady
/// clock nanoseconds; the driver cache saver runs its periodic save only after a few idle seconds.
inline std::atomic<u64> g_last_create_ns{0};
inline void NoteCreate() {
    g_last_create_ns.store(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count()),
                           std::memory_order_relaxed);
}
inline u64 LastCreateNs() {
    return g_last_create_ns.load(std::memory_order_relaxed);
}

template <typename Make, typename Name>
auto Create(Kind kind, u32 stage_count, const void* next, bool probe_ok, Make&& make,
            Name&& name) {
    if (probe_ok && ProbeEnabled()) {
        Call probe{kind, stage_count, next};
        auto out = make(probe.Next(), true);
        probe.Done(out.first, name);
        NoteProbe(out.first);
        if (out.first == vk::Result::eSuccess) {
            return out;
        }
    }
    Call call{kind, stage_count, next};
    auto out = make(call.Next(), false);
    call.Done(out.first, name);
    NoteCreate();
    return out;
}

/// Logs a pipeline that was built on a thread other than a pipeline worker (the command
/// processor or the recorder), once per pipeline when `once` is given.
template <typename Name>
void NoteBuilt(Kind kind, std::atomic<bool>* once, vk::Result result, Name&& name) {
    if (!CompileCheckOn() || result != vk::Result::eSuccess || OnPipelineWorker()) {
        return;
    }
    LogOnce(kind, "built on a non-worker thread", once, name());
}

/// Logs a pipeline the recorder had to wait for, once per pipeline.
template <typename Name>
void NoteWait(Kind kind, std::atomic<bool>* once, Name&& name) {
    if (!CompileCheckOn()) {
        return;
    }
    LogOnce(kind, "recorder waited for it (not ready in time)", once, name());
}

} // namespace Vulkan::SotcPipeFeedback

// sotc: report, called every 5 s by driver.cpp next to the other pipeline reports. Prints nothing
// while both switches are off.
namespace Vulkan {
void ReportPipeFeedback(std::string& out, double seconds);
} // namespace Vulkan
