// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: N5 parallel recording counters (vk_parallel_record.h). The pass decisions are in
// vk_recorder.cpp (PassSecondary); nothing here changes what is recorded.

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_parallel_record.h"
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE mode

namespace Vulkan::ParallelRecord {

namespace {

constexpr std::size_t kBlockCategories = 16; ///< CommandRecorder::BlockCategory::Count (<= 16)

struct Counters {
    std::atomic<u64> parallel{};                            ///< passes handed to a worker
    std::array<std::atomic<u64>, static_cast<size_t>(Fallback::Count)> fallback{};
    std::array<std::atomic<u64>, kBlockCategories> block{}; ///< ScanBlock, by blocking category
    std::atomic<u64> jobs{};                                ///< passes recorded by a worker
    std::atomic<u64> ops{};                                 ///< commands replayed by the workers
    std::atomic<u64> worker_ns{};                           ///< worker time in Record
    std::atomic<u64> owner_wait_ns{};                       ///< owner time waiting for a worker
};
Counters cnt;

// Names of the BlockCategory values (vk_recorder.h), by index.
constexpr std::array<const char*, kBlockCategories> kBlockNames{
    "draw",  "dispatch", "beginrender", "endrender", "barrier", "copy",  "bindpipe", "binddesc",
    "pushc", "dynstate", "vtxidx",      "other",     "label",    "-",     "-",        "-",
};

constexpr std::array<const char*, static_cast<size_t>(Fallback::Count)> kFallbackNames{
    "formats", "dropped", "stopped", "timeout", "chunks", "block", "fewdraws",
};

u64 Take(std::atomic<u64>& a) {
    return a.exchange(0, std::memory_order_relaxed);
}

/// SOTC_N5_PARALLEL=1 with its prerequisites. Read once; a missing prerequisite is logged once.
bool SwitchOn() {
    static const bool value = [] {
        const char* on = std::getenv("SOTC_N5_PARALLEL");
        if (!on || on[0] != '1') {
            return false;
        }
        const auto flag = [](const char* name, const char* want) {
            const char* v = std::getenv(name);
            return v && std::string_view{v} == want;
        };
        const char* graph = std::getenv("SOTC_N5_GRAPH");
        const bool ok_graph = graph && graph[0] != '0';
        const bool ok = SotcNative::On() && ok_graph && flag("SOTC_PARALLEL_RECORD", "1") &&
                        flag("SOTC_PASS_SEC", "1");
        if (!ok) {
            LOG_WARNING(Render,
                        "SOTC_N5_PARALLEL: off, needs SOTC_NATIVE=1, SOTC_N5_GRAPH=1, "
                        "SOTC_PARALLEL_RECORD=1 and SOTC_PASS_SEC=1");
        }
        return ok;
    }();
    return value;
}

} // namespace

bool Enabled() {
    return SwitchOn();
}

void NoteFallback(Fallback reason, u8 block) noexcept {
    cnt.fallback[static_cast<size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    if (reason == Fallback::ScanBlock && block < kBlockCategories) {
        cnt.block[block].fetch_add(1, std::memory_order_relaxed);
    }
}

void NotePassParallel() noexcept {
    cnt.parallel.fetch_add(1, std::memory_order_relaxed);
}

void NoteWorkerJob(u64 ops, u64 ns) noexcept {
    cnt.jobs.fetch_add(1, std::memory_order_relaxed);
    cnt.ops.fetch_add(ops, std::memory_order_relaxed);
    cnt.worker_ns.fetch_add(ns, std::memory_order_relaxed);
}

void NoteOwnerWait(u64 ns) noexcept {
    cnt.owner_wait_ns.fetch_add(ns, std::memory_order_relaxed);
}

void Report(std::string& out, double seconds) {
    if (!Enabled()) {
        return;
    }
    const u64 parallel = Take(cnt.parallel);
    std::array<u64, static_cast<size_t>(Fallback::Count)> fallback{};
    u64 owner = 0;
    for (size_t i = 0; i < fallback.size(); ++i) {
        fallback[i] = Take(cnt.fallback[i]);
        owner += fallback[i];
    }
    std::array<u64, kBlockCategories> block{};
    for (size_t i = 0; i < block.size(); ++i) {
        block[i] = Take(cnt.block[i]);
    }
    const u64 jobs = Take(cnt.jobs);
    const u64 ops = Take(cnt.ops);
    const u64 worker_ns = Take(cnt.worker_ns);
    const u64 owner_wait_ns = Take(cnt.owner_wait_ns);
    if (parallel == 0 && owner == 0) {
        return;
    }

    char line[1024];
    int n = std::snprintf(
        line, sizeof(line),
        "N5 parallel (SOTC_N5_PARALLEL=1): passes secondary %llu, owner %llu (",
        static_cast<unsigned long long>(parallel), static_cast<unsigned long long>(owner));
    for (size_t i = 0; i < fallback.size() && n > 0 && n < int(sizeof(line)); ++i) {
        n += std::snprintf(line + n, sizeof(line) - n, "%s%s %llu", i ? ", " : "",
                           kFallbackNames[i], static_cast<unsigned long long>(fallback[i]));
    }
    if (n > 0 && n < int(sizeof(line))) {
        n += std::snprintf(line + n, sizeof(line) - n, "), block ");
    }
    for (size_t i = 0; i < block.size() && n > 0 && n < int(sizeof(line)); ++i) {
        if (block[i] != 0) {
            n += std::snprintf(line + n, sizeof(line) - n, "%s %llu ", kBlockNames[i],
                               static_cast<unsigned long long>(block[i]));
        }
    }
    const double sec = seconds > 0.0 ? seconds : 1.0;
    const double ops_per_job = jobs ? static_cast<double>(ops) / static_cast<double>(jobs) : 0.0;
    const double worker_ms_per_job =
        jobs ? static_cast<double>(worker_ns) / 1e6 / static_cast<double>(jobs) : 0.0;
    if (n > 0 && n < int(sizeof(line))) {
        std::snprintf(line + n, sizeof(line) - n,
                      "| jobs %llu, ops/job %.1f, worker %.3f ms/job (%.2f%% of 1 worker), owner "
                      "wait %.3f ms/s, workers 1\n",
                      static_cast<unsigned long long>(jobs), ops_per_job, worker_ms_per_job,
                      100.0 * static_cast<double>(worker_ns) / 1e9 / sec,
                      static_cast<double>(owner_wait_ns) / 1e6 / sec);
    }
    out += line;
}

} // namespace Vulkan::ParallelRecord
