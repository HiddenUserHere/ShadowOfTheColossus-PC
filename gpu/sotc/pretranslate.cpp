// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PRETRANSLATE, see pretranslate.h.

#include "pretranslate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace SotcPretranslate {
namespace {

std::mutex g_mutex;
std::vector<Request> g_queue;
std::unordered_set<uint64_t> g_seen;
std::atomic<bool> g_pending{false};
std::atomic<int64_t> g_last_request_ns{0};

int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Counters {
    std::atomic<uint64_t> queued{}, known{}, unpaired{}, built{}, failed{}, superseded{}, used{}, hits{};
    std::array<std::atomic<uint64_t>, DiffBits> diff{};
};
std::array<Counters, 2> g_counters{};

Counters& C(int stage) {
    return g_counters[stage == 1 ? 1 : 0];
}

} // namespace

bool Precompile() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PRECOMPILE");
        return v && v[0] == '1';
    }();
    return on;
}

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PRETRANSLATE");
        return (v && v[0] == '1') || Precompile();
    }();
    return on;
}

uint64_t JobsQueued() noexcept {
    return g_counters[0].queued.load() + g_counters[1].queued.load();
}

uint64_t JobsDone() noexcept {
    return g_counters[0].built.load() + g_counters[0].failed.load() + g_counters[1].built.load() +
           g_counters[1].failed.load();
}

int64_t LastRequestNs() noexcept {
    return g_last_request_ns.load(std::memory_order_relaxed);
}

void Push(Request&& request) noexcept {
    if (!On() || request.code.empty()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_seen.insert(request.hash).second) {
        return;
    }
    g_queue.push_back(std::move(request));
    g_pending.store(true, std::memory_order_release);
    g_last_request_ns.store(SteadyNs(), std::memory_order_relaxed);
}

bool Pending() noexcept {
    return g_pending.load(std::memory_order_acquire);
}

std::vector<Request> Take() {
    std::vector<Request> out;
    std::scoped_lock lock{g_mutex};
    out.swap(g_queue);
    g_pending.store(false, std::memory_order_release);
    return out;
}

void NoteQueued(int stage) noexcept {
    C(stage).queued.fetch_add(1, std::memory_order_relaxed);
}

void NoteDropped(int stage, int reason) noexcept {
    (reason == 0 ? C(stage).known : C(stage).unpaired).fetch_add(1, std::memory_order_relaxed);
}

void NoteBuilt(int stage, bool ok) noexcept {
    (ok ? C(stage).built : C(stage).failed).fetch_add(1, std::memory_order_relaxed);
}

void NoteSuperseded(int stage) noexcept {
    C(stage).superseded.fetch_add(1, std::memory_order_relaxed);
}

void NoteFirstUse(int stage, bool hit, uint32_t diff) noexcept {
    auto& c = C(stage);
    c.used.fetch_add(1, std::memory_order_relaxed);
    if (hit) {
        c.hits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (int b = 0; b < DiffBits; ++b) {
        if ((diff >> b) & 1u) {
            c.diff[b].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Report(std::string& out, double) {
    if (!On()) {
        return;
    }
    static constexpr const char* kStage[2] = {"pixel", "vertex"};
    static constexpr const char* kDiff[DiffBits] = {
        "props",       "layout",      "ps inputs",  "ps other",   "vs hw",         "vs sw",
        "fetch",       "buffers",     "images",     "samplers",   "start",         "fmasks",
        "(z export",   "front face",  "dual source", "clip",      "early z)",      "(img type",
        "integer",     "srgb",        "swizzle",    "conversion", "bindings",      "storage)",
        "(vs outputs", "clip off",    "depth emu",  "user clip",  "step rate",     "vertex sgpr",
        "instance sgpr", "tess emu)"};
    for (int s = 0; s < 2; ++s) {
        const auto& c = g_counters[s];
        char line[1536];
        int n = std::snprintf(
            line, sizeof line,
            "SOTC PRETRANSLATE %s: queued %llu (known %llu, unpaired %llu), built %llu, failed %llu, superseded "
            "%llu; first draws %llu, key hit %llu; misses by key part:",
            kStage[s], (unsigned long long)c.queued.load(), (unsigned long long)c.known.load(),
            (unsigned long long)c.unpaired.load(), (unsigned long long)c.built.load(),
            (unsigned long long)c.failed.load(), (unsigned long long)c.superseded.load(),
            (unsigned long long)c.used.load(), (unsigned long long)c.hits.load());
        for (int b = 0; b < DiffBits && n > 0 && n < static_cast<int>(sizeof line) - 32; ++b) {
            if (const uint64_t v = c.diff[b].load()) {
                n += std::snprintf(line + n, sizeof line - n, " %s %llu", kDiff[b], (unsigned long long)v);
            }
        }
        out += line;
        out += '\n';
    }
}

} // namespace SotcPretranslate
