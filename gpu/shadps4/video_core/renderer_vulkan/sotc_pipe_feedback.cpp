// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: pipeline compile measurement (SOTC_PIPE_FEEDBACK, SOTC_PIPE_COMPILE_CHECK). See the header.

#include <cstdio>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "common/thread.h"
#include "video_core/renderer_vulkan/sotc_pipe_feedback.h"
#include "video_core/sotc_stats.h"

namespace Vulkan::SotcPipeFeedback {

namespace {

constexpr size_t TopCount = 20;
constexpr size_t NumBins = 6;
// Histogram edges in ns: <1, <10, <50, <200, <1000, >=1000 ms.
constexpr u64 BinEdges[NumBins - 1] = {1'000'000, 10'000'000, 50'000'000, 200'000'000,
                                       1'000'000'000};
constexpr const char* KindNames[static_cast<size_t>(Kind::Count)] = {
    "compute", "graphics", "gpl library", "gpl link"};

struct KindStats {
    std::atomic<u64> count{};
    std::atomic<u64> failed{};
    std::atomic<u64> hits{};
    std::atomic<u64> driver_ns{};
    std::atomic<u64> driver_max_ns{};
    std::atomic<u64> wall_ns{};
    std::atomic<u64> wall_max_ns{};
    std::array<std::atomic<u64>, NumBins> bins{};
};

KindStats g_stats[static_cast<size_t>(Kind::Count)];
std::atomic<u64> g_probe_hit{};
std::atomic<u64> g_probe_miss{};
std::atomic<u64> g_probe_fail{};

struct TopEntry {
    Kind kind;
    u64 driver_ns;
    u64 wall_ns;
    bool hit;
    std::string name;
};

std::mutex g_top_mutex;
std::vector<TopEntry> g_top; // sorted by driver time, slowest first, at most TopCount

// SOTC_PIPE_FEEDBACK_OUT: the TSV file. Opened on the first pipeline, one mutex for all threads.
constexpr const char* KindTokens[static_cast<size_t>(Kind::Count)] = {
    "compute", "graphics", "gpl_library", "gpl_link"};
std::mutex g_tsv_mutex;
std::FILE* g_tsv = nullptr;
bool g_tsv_failed = false;
std::chrono::steady_clock::time_point g_tsv_flushed{};

// Tabs and line breaks would break the TSV columns.
std::string TsvField(std::string text) {
    for (char& c : text) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return text;
}

void AtomicMax(std::atomic<u64>& target, u64 value) noexcept {
    u64 seen = target.load(std::memory_order_relaxed);
    while (value > seen &&
           !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

size_t BinOf(u64 ns) noexcept {
    size_t bin = 0;
    while (bin < NumBins - 1 && ns >= BinEdges[bin]) {
        ++bin;
    }
    return bin;
}

} // namespace

void Record(Kind kind, u64 driver_ns, u64 wall_ns, bool hit) noexcept {
    auto& s = g_stats[static_cast<size_t>(kind)];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.driver_ns.fetch_add(driver_ns, std::memory_order_relaxed);
    AtomicMax(s.driver_max_ns, driver_ns);
    s.wall_ns.fetch_add(wall_ns, std::memory_order_relaxed);
    AtomicMax(s.wall_max_ns, wall_ns);
    if (hit) {
        s.hits.fetch_add(1, std::memory_order_relaxed);
    }
    s.bins[BinOf(driver_ns)].fetch_add(1, std::memory_order_relaxed);
}

void CountFailed(Kind kind) noexcept {
    g_stats[static_cast<size_t>(kind)].failed.fetch_add(1, std::memory_order_relaxed);
}

void AddTop(Kind kind, u64 driver_ns, u64 wall_ns, bool hit, std::string name) {
    std::lock_guard lock{g_top_mutex};
    const auto pos = std::find_if(g_top.begin(), g_top.end(), [&](const TopEntry& e) {
        return e.driver_ns < driver_ns;
    });
    g_top.insert(pos, TopEntry{kind, driver_ns, wall_ns, hit, std::move(name)});
    if (g_top.size() > TopCount) {
        g_top.pop_back();
    }
    g_top_threshold_ns.store(g_top.size() < TopCount ? 0 : g_top.back().driver_ns,
                             std::memory_order_relaxed);
}

// sotc: one TSV line per created pipeline (SOTC_PIPE_FEEDBACK_OUT, header in the .h).
void AppendTsv(Kind kind, const std::string& name, u64 driver_ns, u64 wall_ns, bool hit) {
    std::lock_guard lock{g_tsv_mutex};
    if (g_tsv == nullptr) {
        if (g_tsv_failed) {
            return;
        }
        g_tsv = std::fopen(TsvPath(), "w");
        if (g_tsv == nullptr) {
            g_tsv_failed = true;
            std::fprintf(stderr, "SOTC PIPE FEEDBACK: cannot open %s\n", TsvPath());
            return;
        }
        std::fputs("kind\tname\tdriver_ms\twall_ms\tthread\thit\n", g_tsv);
    }
    std::fprintf(g_tsv, "%s\t%s\t%.3f\t%.3f\t%s\t%d\n", KindTokens[static_cast<size_t>(kind)],
                 TsvField(name).c_str(), driver_ns / 1e6, wall_ns / 1e6,
                 TsvField(Common::GetCurrentThreadName()).c_str(), hit ? 1 : 0);
    const auto now = std::chrono::steady_clock::now();
    if (now - g_tsv_flushed >= std::chrono::seconds{1}) {
        std::fflush(g_tsv);
        g_tsv_flushed = now;
    }
}

void NoteProbe(vk::Result result) noexcept {
    if (result == vk::Result::eSuccess) {
        g_probe_hit.fetch_add(1, std::memory_order_relaxed);
    } else if (result == vk::Result::ePipelineCompileRequiredEXT) {
        g_probe_miss.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_probe_fail.fetch_add(1, std::memory_order_relaxed);
    }
}

bool OnPipelineWorker() {
    return Common::GetCurrentThreadName().starts_with("shadPS4:PipelineWorker");
}

void LogOnce(Kind kind, const char* what, std::atomic<bool>* once, const std::string& name) {
    if (once != nullptr && once->exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr, "SOTC PIPE COMPILE CHECK %s %s: %s\n",
                 KindNames[static_cast<size_t>(kind)], what, name.c_str());
}

void Report(std::string& out) {
    const bool feedback = FeedbackOn();
    const bool check = CompileCheckOn();
    if (!feedback && !check) {
        return;
    }

    std::string line = "SOTC PIPE FEEDBACK:";
    if (feedback) {
        for (size_t k = 0; k < static_cast<size_t>(Kind::Count); ++k) {
            const auto& s = g_stats[k];
            const double driver_ms = s.driver_ns.load() / 1e6;
            const double wall_ms = s.wall_ns.load() / 1e6;
            line += fmt::format(
                "{}{} {} (driver {:.1f} ms total, max {:.1f} ms, wall {:.1f} ms, cache hits {}, "
                "failed {}, histogram <1 {}, <10 {}, <50 {}, <200 {}, <1000 {}, >=1000 {})",
                k == 0 ? " " : ", ", KindNames[k], s.count.load(), driver_ms,
                s.driver_max_ns.load() / 1e6, wall_ms, s.hits.load(), s.failed.load(),
                s.bins[0].load(), s.bins[1].load(), s.bins[2].load(), s.bins[3].load(),
                s.bins[4].load(), s.bins[5].load());
        }
        line += ";";
    }
    line += fmt::format(" recorder waits {} ({:.1f} ms)",
                        SotcStats::async_compute_waits.load(std::memory_order_relaxed),
                        SotcStats::async_compute_wait_ns.load(std::memory_order_relaxed) / 1e6);
    if (check) {
        line += fmt::format("; driver cache miss {} hit {} failed {} (probe {})",
                            g_probe_miss.load(), g_probe_hit.load(), g_probe_fail.load(),
                            ProbeEnabled() ? "on"
                                           : "off: pipelineCreationCacheControl not enabled");
    }
    line += "\n";
    out += line;

    // The 20 slowest pipelines: at the first report that has entries, then every 30 s. The old gate
    // counted 30 s from the first Report call, and the cold runs (15-25 s of reports) never reached
    // it. Report runs on the flip thread only, so these statics need no lock.
    static bool top_printed = false;
    static auto last_top = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (!feedback || (top_printed && now - last_top < std::chrono::seconds{30})) {
        return;
    }
    std::vector<TopEntry> top;
    {
        std::lock_guard lock{g_top_mutex};
        top = g_top;
    }
    if (top.empty()) {
        return;
    }
    top_printed = true;
    last_top = now;
    out += fmt::format("SOTC PIPE FEEDBACK TOP {} SLOWEST (driver ms, wall ms, kind, cache hit, "
                       "pipeline):\n",
                       top.size());
    for (size_t i = 0; i < top.size(); ++i) {
        const auto& e = top[i];
        out += fmt::format("  #{} driver {:.1f} ms, wall {:.1f} ms, {}, hit {}: {}\n", i + 1,
                           e.driver_ns / 1e6, e.wall_ns / 1e6,
                           KindNames[static_cast<size_t>(e.kind)], e.hit ? 1 : 0, e.name);
    }
}

} // namespace Vulkan::SotcPipeFeedback

namespace Vulkan {

void ReportPipeFeedback(std::string& out, double seconds) {
    (void)seconds;
    SotcPipeFeedback::Report(out);
}

} // namespace Vulkan
