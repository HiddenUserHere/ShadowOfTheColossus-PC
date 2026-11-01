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
    std::atomic<u64> would{};                               ///< mode 2: passes that would go parallel
    std::atomic<u64> would_draws{};                         ///< mode 2: draws in those passes
    std::atomic<u64> would_blocks{};                        ///< mode 2: command blocks in those passes
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

/// SOTC_N5_PARALLEL=1 (parallel) or 2 (dry run) with its prerequisites. Read once; a missing
/// prerequisite is logged once and gives mode 0.
int SwitchOn() {
    static const int value = [] {
        const char* on = std::getenv("SOTC_N5_PARALLEL");
        if (!on || (on[0] != '1' && on[0] != '2')) {
            return 0;
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
            return 0;
        }
        LOG_INFO(Render, "SOTC_N5_PARALLEL={}: {}", on[0],
                 on[0] == '2' ? "dry run, passes are counted as would-parallel and recorded serially"
                              : "passes are recorded by the pass worker, counters on");
        return on[0] - '0';
    }();
    return value;
}

} // namespace

int Mode() {
    return SwitchOn();
}

bool Enabled() {
    return SwitchOn() != 0;
}

bool Dry() {
    return SwitchOn() == 2;
}

void NoteWouldParallel(u64 draws, u64 blocks) noexcept {
    cnt.would.fetch_add(1, std::memory_order_relaxed);
    cnt.would_draws.fetch_add(draws, std::memory_order_relaxed);
    cnt.would_blocks.fetch_add(blocks, std::memory_order_relaxed);
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
    const u64 would = Take(cnt.would);
    const u64 would_draws = Take(cnt.would_draws);
    const u64 would_blocks = Take(cnt.would_blocks);
    if (parallel == 0 && owner == 0 && would == 0) {
        return;
    }
    const bool dry = Dry();

    char line[1024];
    int n = std::snprintf(
        line, sizeof(line),
        "N5 parallel (SOTC_N5_PARALLEL=%d%s): passes secondary %llu, owner %llu (",
        dry ? 2 : 1, dry ? ", dry run" : "", static_cast<unsigned long long>(parallel),
        static_cast<unsigned long long>(owner));
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
    if (dry) {
        // Mode 2: nothing ran on a worker; would-parallel passes are the owner's, recorded serially
        const double per_pass =
            would ? static_cast<double>(would_draws) / static_cast<double>(would) : 0.0;
        if (n > 0 && n < int(sizeof(line))) {
            std::snprintf(line + n, sizeof(line) - n,
                          "| would parallel %llu passes (%.1f/s, draws %llu, %.1f draws/pass, blocks "
                          "%llu), workers 0\n",
                          static_cast<unsigned long long>(would), static_cast<double>(would) / sec,
                          static_cast<unsigned long long>(would_draws), per_pass,
                          static_cast<unsigned long long>(would_blocks));
        }
    } else if (n > 0 && n < int(sizeof(line))) {
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
