// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: N5 parallel recording counters (vk_parallel_record.h). The pass decisions are in
// vk_recorder.cpp (PassSecondary); nothing here changes what is recorded.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_parallel_record.h"
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE mode
#include "video_core/sotc_arch/sotc_arch.h" // sotc: SOTC_PACKET and SOTC_PASS_RECORD accessors

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
    // SOTC_N5_PARALLEL_TRANSLATE census: producer (draw thread) emission into the stream
    std::atomic<u64> emit_blocks{};
    std::atomic<u64> emit_bytes{};
    std::atomic<u64> emit_ns{};
    std::array<std::atomic<u64>, kBlockCategories> emit_by_category{};
    // sotc: SOTC_PASS_RECORD + SOTC_PACKET (PacketRecord): passes that passed the scan
    std::atomic<u64> packet_passes{};           ///< scanned passes
    std::atomic<u64> packet_passes_free{};      ///< of those, passes that bind no descriptor set
    std::atomic<u64> packet_draws{};            ///< draws in the scanned passes
    std::atomic<u64> stitch_ns{};               ///< owner: execute secondary + replay at EndRendering
    std::atomic<u64> stitch_count{};            ///< owner: stitches timed
    // sotc: SOTC_PASS_WORKERS / SOTC_PASS_VERIFY (ReportPool)
    std::array<std::atomic<u64>, static_cast<size_t>(LaStop::Count)> la_stop{};
    std::atomic<u64> la_prepared{};             ///< passes the look-ahead handed to a worker
    std::atomic<u64> la_depth_sum{};            ///< sum of the open passes at each prepare
    std::atomic<u64> la_depth_max{};            ///< largest number of open passes at a prepare
    std::atomic<u64> verify_checked{};          ///< jobs checked at their EndRendering
    std::atomic<u64> verify_state_bad{};        ///< snapshot differs from the consumer table
    std::atomic<u64> verify_run_bad{};          ///< worker block hash differs from the reference
};
Counters cnt;

// Names of the BlockCategory values (vk_recorder.h), by index.
constexpr std::array<const char*, kBlockCategories> kBlockNames{
    "draw",  "dispatch", "beginrender", "endrender", "barrier", "copy",  "bindpipe", "binddesc",
    "pushc", "dynstate", "vtxidx",      "other",     "label",    "-",     "-",        "-",
};

constexpr std::array<const char*, static_cast<size_t>(Fallback::Count)> kFallbackNames{
    "formats", "dropped", "stopped", "timeout", "chunks", "block", "fewdraws", "workers", "ring",
};

constexpr std::array<const char*, static_cast<size_t>(LaStop::Count)> kLaNames{
    "other", "prune", "queue", "ring", "incomplete",
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

/// SOTC_N5_PARALLEL_TRANSLATE=1 turns on the producer emission census (NoteEmit). Read once.
bool TranslateOn() {
    static const bool value = [] {
        const char* v = std::getenv("SOTC_N5_PARALLEL_TRANSLATE");
        const bool on = v && v[0] == '1';
        if (on) {
            LOG_INFO(Render, "SOTC_N5_PARALLEL_TRANSLATE=1: producer emission census on, no op is "
                             "translated off the draw thread");
        }
        return on;
    }();
    return value;
}

/// One 5 s line for the census window: what the draw thread spends writing command blocks.
void ReportEmit(std::string& out, double seconds) {
    if (!TranslateOn()) {
        return;
    }
    const u64 blocks = Take(cnt.emit_blocks);
    const u64 bytes = Take(cnt.emit_bytes);
    const u64 ns = Take(cnt.emit_ns);
    std::array<u64, kBlockCategories> by{};
    for (size_t i = 0; i < by.size(); ++i) {
        by[i] = Take(cnt.emit_by_category[i]);
    }
    if (blocks == 0) {
        return;
    }
    const double sec = seconds > 0.0 ? seconds : 1.0;
    char line[1024];
    int n = std::snprintf(
        line, sizeof(line),
        "N5 translate (SOTC_N5_PARALLEL_TRANSLATE=1): emit %.0f blocks/s, %.2f MiB/s, producer "
        "emit %.3f ms/s (%.2f%% of the window), by category ",
        static_cast<double>(blocks) / sec, static_cast<double>(bytes) / sec / 1048576.0,
        static_cast<double>(ns) / 1e6 / sec, 100.0 * static_cast<double>(ns) / 1e9 / sec);
    for (size_t i = 0; i < by.size() && n > 0 && n < int(sizeof(line)); ++i) {
        if (by[i] != 0) {
            n += std::snprintf(line + n, sizeof(line) - n, "%s %.0f/s ", kBlockNames[i],
                               static_cast<double>(by[i]) / sec);
        }
    }
    if (n > 0 && n < int(sizeof(line))) {
        std::snprintf(line + n, sizeof(line) - n, "| resolution not counted, workers 0\n");
    }
    out += line;
}

/// One 5 s line for the packet-pass window: the passes the worker recording of packet ops would take,
/// and what the owner spends stitching a secondary into the stream.
void ReportPacket(std::string& out, double seconds) {
    if (!PacketRecord()) {
        return;
    }
    const u64 passes = Take(cnt.packet_passes);
    const u64 free = Take(cnt.packet_passes_free);
    const u64 draws = Take(cnt.packet_draws);
    const u64 stitch_ns = Take(cnt.stitch_ns);
    const u64 stitches = Take(cnt.stitch_count);
    if (passes == 0 && stitches == 0) {
        return;
    }
    const double sec = seconds > 0.0 ? seconds : 1.0;
    char line[512];
    std::snprintf(
        line, sizeof(line),
        "N5 packet (SOTC_PASS_RECORD=1, SOTC_PACKET=1): passes %.1f/s, no descriptor bind %.1f/s, "
        "draws %.1f/pass, stitch %.1f us/pass (%.3f ms/s), workers 0\n",
        static_cast<double>(passes) / sec, static_cast<double>(free) / sec,
        passes ? static_cast<double>(draws) / static_cast<double>(passes) : 0.0,
        stitches ? static_cast<double>(stitch_ns) / 1e3 / static_cast<double>(stitches) : 0.0,
        static_cast<double>(stitch_ns) / 1e6 / sec);
    out += line;
}

/// sotc: SOTC_PASS_WORKERS / SOTC_PASS_VERIFY, one line for the pool window (it replaces the N5 line
/// in Report) and one for the verify checks. Takes every counter of the window.
void ReportPool(std::string& out, double seconds, int workers) {
    const u64 parallel = Take(cnt.parallel);
    std::array<u64, static_cast<size_t>(Fallback::Count)> fallback{};
    u64 inline_total = 0;
    for (size_t i = 0; i < fallback.size(); ++i) {
        fallback[i] = Take(cnt.fallback[i]);
        inline_total += fallback[i];
    }
    std::array<u64, kBlockCategories> block{};
    for (size_t i = 0; i < block.size(); ++i) {
        block[i] = Take(cnt.block[i]);
    }
    const u64 jobs = Take(cnt.jobs);
    const u64 ops = Take(cnt.ops);
    const u64 worker_ns = Take(cnt.worker_ns);
    const u64 owner_wait_ns = Take(cnt.owner_wait_ns);
    std::array<u64, static_cast<size_t>(LaStop::Count)> stops{};
    u64 stop_total = 0;
    for (size_t i = 0; i < stops.size(); ++i) {
        stops[i] = Take(cnt.la_stop[i]);
        stop_total += stops[i];
    }
    const u64 prepared = Take(cnt.la_prepared);
    const u64 depth_sum = Take(cnt.la_depth_sum);
    const u64 depth_max = Take(cnt.la_depth_max);
    const u64 checked = Take(cnt.verify_checked);
    const u64 state_bad = Take(cnt.verify_state_bad);
    const u64 run_bad = Take(cnt.verify_run_bad);
    if (parallel == 0 && inline_total == 0 && stop_total == 0 && prepared == 0 && checked == 0) {
        return;
    }
    const double sec = seconds > 0.0 ? seconds : 1.0;
    const double workers_d = workers > 0 ? static_cast<double>(workers) : 1.0;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "N5 pool (SOTC_PASS_WORKERS=%d, look-ahead %s): passes on workers %llu (%.1f/s), "
                  "inline %llu (",
                  workers, workers >= 2 ? "on" : "off", static_cast<unsigned long long>(parallel),
                  static_cast<double>(parallel) / sec, static_cast<unsigned long long>(inline_total));
    out += buf;
    for (size_t i = 0; i < fallback.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s%s %llu", i ? ", " : "", kFallbackNames[i],
                      static_cast<unsigned long long>(fallback[i]));
        out += buf;
    }
    out += "), block ";
    for (size_t i = 0; i < block.size(); ++i) {
        if (block[i] != 0) {
            std::snprintf(buf, sizeof(buf), "%s %llu ", kBlockNames[i],
                          static_cast<unsigned long long>(block[i]));
            out += buf;
        }
    }
    const double avg_depth = prepared ? static_cast<double>(depth_sum) / static_cast<double>(prepared) : 0.0;
    std::snprintf(buf, sizeof(buf),
                  "| look-ahead prepared %llu, open passes avg %.2f max %llu, stop events (",
                  static_cast<unsigned long long>(prepared), avg_depth,
                  static_cast<unsigned long long>(depth_max));
    out += buf;
    for (size_t i = 0; i < stops.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s%s %llu", i ? ", " : "", kLaNames[i],
                      static_cast<unsigned long long>(stops[i]));
        out += buf;
    }
    const double ops_per_job = jobs ? static_cast<double>(ops) / static_cast<double>(jobs) : 0.0;
    const double worker_ms_per_job =
        jobs ? static_cast<double>(worker_ns) / 1e6 / static_cast<double>(jobs) : 0.0;
    std::snprintf(buf, sizeof(buf),
                  ") | jobs %llu, ops/job %.1f, worker %.3f ms/job (%.1f%% busy of %d), owner wait "
                  "%.3f ms/s\n",
                  static_cast<unsigned long long>(jobs), ops_per_job, worker_ms_per_job,
                  100.0 * static_cast<double>(worker_ns) / 1e9 / sec / workers_d, workers,
                  static_cast<double>(owner_wait_ns) / 1e6 / sec);
    out += buf;
    if (Verify()) {
        std::snprintf(buf, sizeof(buf),
                      "N5 verify (SOTC_PASS_VERIFY=1): checked %llu, state mismatch %llu, block hash "
                      "mismatch %llu (the parallel result is used)\n",
                      static_cast<unsigned long long>(checked),
                      static_cast<unsigned long long>(state_bad),
                      static_cast<unsigned long long>(run_bad));
        out += buf;
    }
}

} // namespace

bool PacketRecord() {
    static const bool value = [] {
        const bool on = SotcArch::Packet::On() && SotcArch::Passes::ParallelRecord();
        if (on) {
            LOG_INFO(Render, "SOTC_PASS_RECORD=1 with SOTC_PACKET=1: packet passes are counted, "
                             "stitch timed");
        }
        return on;
    }();
    return value;
}

void NotePacketPass(bool descriptor_free, u64 draws) noexcept {
    cnt.packet_passes.fetch_add(1, std::memory_order_relaxed);
    if (descriptor_free) {
        cnt.packet_passes_free.fetch_add(1, std::memory_order_relaxed);
    }
    cnt.packet_draws.fetch_add(draws, std::memory_order_relaxed);
}

void NoteStitch(u64 ns) noexcept {
    cnt.stitch_ns.fetch_add(ns, std::memory_order_relaxed);
    cnt.stitch_count.fetch_add(1, std::memory_order_relaxed);
}

int Workers() {
    static const int value = [] {
        const char* v = std::getenv("SOTC_PASS_WORKERS");
        if (!v || !v[0]) {
            return 1;
        }
        const char* sec = std::getenv("SOTC_PASS_SEC");
        if (!sec || sec[0] != '1') {
            LOG_WARNING(Render, "SOTC_PASS_WORKERS: needs SOTC_PASS_SEC=1, one pass worker");
            return 1;
        }
        int n = 1;
        if (std::string_view{v} == "auto") {
            const unsigned cores = std::thread::hardware_concurrency();
            n = static_cast<int>(std::min(4U, std::max(1U, cores / 4)));
        } else {
            n = std::atoi(v);
        }
        n = std::clamp(n, 1, 8);
        LOG_INFO(Render, "SOTC_PASS_WORKERS={}: {} pass worker(s), up to {} passes open at once", v,
                 n, n);
        return n;
    }();
    return value;
}

bool Verify() {
    static const bool value = [] {
        const char* v = std::getenv("SOTC_PASS_VERIFY");
        const bool on = v && v[0] == '1';
        if (on) {
            LOG_INFO(Render, "SOTC_PASS_VERIFY=1: pass jobs checked against the consumer table and a "
                             "block hash; the parallel result is used");
        }
        return on;
    }();
    return value;
}

bool Counted() {
    return Enabled() || Workers() > 1 || Verify();
}

void NoteLookahead(LaStop why) noexcept {
    cnt.la_stop[static_cast<size_t>(why)].fetch_add(1, std::memory_order_relaxed);
}

void NoteLookaheadPrepare(u64 depth) noexcept {
    cnt.la_prepared.fetch_add(1, std::memory_order_relaxed);
    cnt.la_depth_sum.fetch_add(depth, std::memory_order_relaxed);
    u64 seen = cnt.la_depth_max.load(std::memory_order_relaxed);
    while (depth > seen &&
           !cnt.la_depth_max.compare_exchange_weak(seen, depth, std::memory_order_relaxed)) {
    }
}

void NoteVerify(bool state_ok, bool run_ok) noexcept {
    cnt.verify_checked.fetch_add(1, std::memory_order_relaxed);
    if (!state_ok) {
        cnt.verify_state_bad.fetch_add(1, std::memory_order_relaxed);
    }
    if (!run_ok) {
        cnt.verify_run_bad.fetch_add(1, std::memory_order_relaxed);
    }
}

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

bool TranslateCensus() {
    return TranslateOn();
}

u64 NowNs() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void NoteEmit(u8 category, u32 bytes, u64 ns) noexcept {
    cnt.emit_blocks.fetch_add(1, std::memory_order_relaxed);
    cnt.emit_bytes.fetch_add(bytes, std::memory_order_relaxed);
    cnt.emit_ns.fetch_add(ns, std::memory_order_relaxed);
    if (category < kBlockCategories) {
        cnt.emit_by_category[category].fetch_add(1, std::memory_order_relaxed);
    }
}

void Report(std::string& out, double seconds) {
    ReportEmit(out, seconds);
    ReportPacket(out, seconds); // sotc: SOTC_PASS_RECORD + SOTC_PACKET
    if (Workers() > 1 || Verify()) {
        // sotc: SOTC_PASS_WORKERS / SOTC_PASS_VERIFY: the pool line replaces the N5 line
        ReportPool(out, seconds, Workers());
        return;
    }
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
