// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_* stage modes, refusals, the report and the submit-start hook (gcn_native.h). Each stage reads its
// switch once, at its first use, and every stage's mode is then one relaxed atomic load.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>

#include "common/logging/log.h"
#include "video_core/sotc_arch/gcn_native.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcGcn {

namespace {

constexpr int kStages = static_cast<int>(Stage::Count);
constexpr int kMem = static_cast<int>(Stage::Mem);
constexpr int kBuf = static_cast<int>(Stage::Buf);
constexpr int kTex = static_cast<int>(Stage::Tex);
constexpr int kSync = static_cast<int>(Stage::Sync);
constexpr int kBarrier = static_cast<int>(Stage::Barrier);
constexpr int kCommit = static_cast<int>(Stage::Commit);

constexpr const char* kNames[kStages] = {"mem", "buf", "tex", "sync", "barrier", "commit"};
constexpr const char* kSwitches[kStages] = {"SOTC_GCN_MEM",  "SOTC_GCN_BUF",     "SOTC_GCN_TEX",
                                            "SOTC_GCN_SYNC", "SOTC_GCN_BARRIER", "SOTC_GCN_COMMIT"};

/// -1: not read yet (ReadAllLocked sets every stage at once). Otherwise 0, 1 or 2.
std::atomic<int> g_mode[kStages]{-1, -1, -1, -1, -1, -1};
/// One refusal line per stage for the run.
std::atomic<bool> g_logged[kStages]{};
/// Serializes the first read and the refusals (never taken on the fast path of Mode).
std::mutex g_mutex;

void LogRefusal(int stage, const char* reason) {
    if (!g_logged[stage].exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render, "SOTC GCN: {} refused: {}", kNames[stage], reason);
    }
}

/// Sets a stage to 0. A stage that was on is logged; a stage that was off stays silent (no refusal happened).
/// g_mutex held.
void LowerLocked(int stage, const char* reason) {
    if (g_mode[stage].exchange(0, std::memory_order_relaxed) != 0) {
        LogRefusal(stage, reason);
    }
}

/// Reads every switch and applies the dependencies. g_mutex held.
void ReadAllLocked() {
    if (g_mode[kMem].load(std::memory_order_relaxed) >= 0) {
        return;
    }
    int modes[kStages];
    for (int i = 0; i < kStages; ++i) {
        modes[i] = SotcArch::SwitchMode(kSwitches[i]);
    }
    // Buf and Sync address memory through the table of Mem: without Mem they cannot run.
    if (modes[kMem] == 0) {
        for (const int dependent : {kBuf, kSync}) {
            if (modes[dependent] != 0) {
                LogRefusal(dependent, "needs SOTC_GCN_MEM, which is off");
                modes[dependent] = 0;
            }
        }
    }
    // S6: the commit lane rides on the prep threads of SOTC_PARALLEL_RECORD, read as liverpool.cpp reads it (= "1").
    if (modes[kCommit] != 0) {
        const char* parallel = std::getenv("SOTC_PARALLEL_RECORD");
        if (!parallel || parallel[0] != '1') {
            LogRefusal(kCommit, "needs SOTC_PARALLEL_RECORD=1, which is off");
            modes[kCommit] = 0;
        }
    }
    for (int i = 0; i < kStages; ++i) {
        g_mode[i].store(modes[i], std::memory_order_relaxed);
    }
}

} // namespace

int Mode(Stage stage) {
    const int i = static_cast<int>(stage);
    const int mode = g_mode[i].load(std::memory_order_relaxed);
    if (mode >= 0) {
        return mode;
    }
    std::scoped_lock lock{g_mutex};
    ReadAllLocked();
    return g_mode[i].load(std::memory_order_relaxed);
}

void Refuse(Stage stage, const char* reason) {
    std::scoped_lock lock{g_mutex};
    ReadAllLocked(); // a refusal before the first read must not be overwritten by it
    LowerLocked(static_cast<int>(stage), reason);
    if (stage == Stage::Mem) {
        LowerLocked(kBuf, "needs SOTC_GCN_MEM, which is refused");
        LowerLocked(kSync, "needs SOTC_GCN_MEM, which is refused");
    }
}

void LogDecisions() {
    LOG_INFO(Render, "SOTC GCN: mem={} buf={} tex={} sync={} barrier={} commit={}", Mode(Stage::Mem),
             Mode(Stage::Buf), Mode(Stage::Tex), Mode(Stage::Sync), Mode(Stage::Barrier), Mode(Stage::Commit));
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    u32 used = 0;
    // Appends one stage's line (already formatted by the stage) with a separator; cut when the buffer is full.
    const auto append = [&](const char* line) {
        if (!line[0]) {
            return;
        }
        if (used != 0 && used + 1 < size) {
            out[used++] = '\n';
        }
        const size_t left = size - 1 - used;
        const size_t n = std::min(std::strlen(line), left);
        std::memcpy(out + used, line, n);
        used += static_cast<u32>(n);
        out[used] = '\0';
    };
    // A stage contributes only while it is on: a run with every mode 0 reports nothing.
    const auto on = [](Stage stage) { return Mode(stage) != 0; };
    char line[512];
    if (on(Stage::Mem) || on(Stage::Buf) || on(Stage::Tex) || on(Stage::Sync) || on(Stage::Barrier) ||
        on(Stage::Commit)) {
        line[0] = '\0';
        if (on(Stage::Buf) || on(Stage::Tex)) {
            Globals::Report(line, sizeof line, seconds);
            append(line);
        }
        if (on(Stage::Mem)) {
            line[0] = '\0';
            Mem::Report(line, sizeof line, seconds);
            append(line);
        }
        if (on(Stage::Buf)) {
            line[0] = '\0';
            Buf::Report(line, sizeof line, seconds);
            append(line);
        }
        if (on(Stage::Tex)) {
            line[0] = '\0';
            Tex::Report(line, sizeof line, seconds);
            append(line);
        }
        if (on(Stage::Sync)) {
            line[0] = '\0';
            Sync::Report(line, sizeof line, seconds);
            append(line);
        }
        if (on(Stage::Barrier)) {
            line[0] = '\0';
            Barrier::Report(line, sizeof line, seconds);
            append(line);
        }
        if (on(Stage::Commit)) {
            line[0] = '\0';
            Commit::Report(line, sizeof line, seconds);
            append(line);
        }
    }
}

namespace {

/// What the miss sink needs: the caches of the submit (draw thread, texture cache lock taken by Tex).
struct MissContext {
    VideoCore::BufferCache& cache;
    VideoCore::TextureCache& texture_cache;
    Vulkan::Scheduler& scheduler;
};

} // namespace

namespace Tex {

bool SkipResolve() {
    // sotc: the rasterizer's former local read (SOTC_GCN_TEX_SKIP_RESOLVE=1), read once.
    static const bool skip = [] {
        const char* v = std::getenv("SOTC_GCN_TEX_SKIP_RESOLVE");
        return v && v[0] == '1';
    }();
    return skip;
}

} // namespace Tex

void OnSubmitStart(VideoCore::BufferCache& cache, VideoCore::TextureCache& texture_cache,
                   Vulkan::Scheduler& scheduler) {
    bool any_on = false;
    for (int i = 0; i < kStages; ++i) {
        any_on = any_on || Mode(static_cast<Stage>(i)) != 0;
    }
    if (!any_on) {
        return; // every mode is 0: nothing to do
    }
    const bool buf_on = Mode(Stage::Buf) != 0;
    const bool tex_on = Mode(Stage::Tex) != 0;
    const bool sync_on = Sync::On();
    if (buf_on || tex_on) {
        MissContext ctx{cache, texture_cache, scheduler};
        Globals::MissSink sink;
        sink.user = &ctx;
        sink.page = [](void* user, VAddr page_addr) {
            auto& c = *static_cast<MissContext*>(user);
            Buf::OnPageMiss(c.cache, c.scheduler, page_addr);
        };
        sink.image = [](void* user, const u32 (&tsharp)[8], u32 view_class) {
            auto& c = *static_cast<MissContext*>(user);
            Tex::OnImageMiss(c.texture_cache, tsharp, view_class);
        };
        sink.sampler = [](void* user, const u32 (&ssharp)[4], u32 sampler_class) {
            auto& c = *static_cast<MissContext*>(user);
            Tex::OnSamplerMiss(c.texture_cache, ssharp, sampler_class);
        };
        Globals::DrainMisses(sink);
    }
    if (tex_on) {
        Tex::StampLiveSlots(); // sotc: the bindless LRU must not evict a slot the table serves
    }
    if (sync_on) {
        Sync::SubmitBatch(cache, texture_cache, scheduler);
    }
}

} // namespace SotcGcn
