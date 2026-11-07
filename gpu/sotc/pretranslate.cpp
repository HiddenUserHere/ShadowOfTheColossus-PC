// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PRETRANSLATE, see pretranslate.h.

#include "pretranslate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "common/hash.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/specialization.h"

namespace SotcPretranslate {
namespace {

std::mutex g_mutex;

// sotc: SOTC_PRETRANSLATE_COMPUTE_HINTS state, guarded by g_hint_mutex (see pretranslate.h).
std::mutex g_hint_mutex;
constexpr const char* kHintPath = "user/shader_predict/compute_hints.tsv";
bool g_hints_loaded = false;
std::unordered_map<uint64_t, std::shared_ptr<Hint>> g_hints;
std::unordered_set<uint64_t> g_hinted;  // prebuilt with a hint, first dispatch not seen yet
std::FILE* g_hint_out = nullptr;
uint64_t g_hint_applied{}, g_hint_first{}, g_hint_first_hit{}, g_hint_written{}, g_hint_new{}, g_miss_logged{};
uint64_t g_hint_lookups{}; // sotc: AcquireHint calls (prebuilds that looked for a hint)
thread_local const Hint* t_active_hint = nullptr;
std::vector<Request> g_queue;
std::unordered_set<uint64_t> g_seen;
std::atomic<bool> g_pending{false};
std::atomic<int64_t> g_last_request_ns{0};

int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Counters {
    std::atomic<uint64_t> queued{}, known{}, unpaired{}, built{}, failed{}, superseded{}, used{}, hits{}, mode{},
        other{};
    std::array<std::atomic<uint64_t>, DiffBits> diff{};
    // Compute only: the layouts of the predicted programs, the register verify at the first dispatch.
    std::array<std::atomic<uint64_t>, 3> layout{};
    std::atomic<uint64_t> ambiguous{}, verify_match{}, verify_mismatch{};
};
constexpr int kStages = 3; // pixel, vertex, compute
std::array<Counters, kStages> g_counters{};

Counters& C(int stage) {
    return g_counters[stage == 2 ? 2 : stage == 1 ? 1 : 0];
}

} // namespace

bool Precompile() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PRECOMPILE");
        return v && v[0] == '1';
    }();
    return on;
}

bool PixelVertexOn() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PRETRANSLATE");
        return (v && v[0] == '1') || Precompile();
    }();
    return on;
}

bool ComputeOn() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PRETRANSLATE_COMPUTE");
        return v && v[0] == '1';
    }();
    return on;
}

bool On() noexcept {
    static const bool on = PixelVertexOn() || ComputeOn();
    return on;
}

uint64_t JobsQueued() noexcept {
    uint64_t n = 0;
    for (const auto& c : g_counters) {
        n += c.queued.load();
    }
    return n;
}

uint64_t JobsDone() noexcept {
    // A compute job is done when its pipeline is built (or failed), so the boot hold waits for the builds too.
    uint64_t n = 0;
    for (const auto& c : g_counters) {
        n += c.built.load() + c.failed.load();
    }
    return n;
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
    auto& c = C(stage);
    (reason == 0 ? c.known : reason == 1 ? c.unpaired : reason == 2 ? c.mode : c.other)
        .fetch_add(1, std::memory_order_relaxed);
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

void NoteComputeLayout(uint8_t layout, bool ambiguous) noexcept {
    C(2).layout[layout < 3 ? layout : 0].fetch_add(1, std::memory_order_relaxed);
    if (ambiguous) {
        C(2).ambiguous.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteVerify(bool match) noexcept {
    (match ? C(2).verify_match : C(2).verify_mismatch).fetch_add(1, std::memory_order_relaxed);
}

// sotc: SOTC_PRETRANSLATE_COMPUTE_HINTS. The hint file is read once, under g_hint_mutex.
namespace {

void LoadHintsLocked() {
    g_hints_loaded = true;
    std::ifstream in(kHintPath);
    if (!in) {
        return;
    }
    std::unordered_map<uint64_t, Hint> loaded;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::istringstream fields(line);
        for (std::string item; std::getline(fields, item, '\t');) {
            f.push_back(item);
        }
        if (f.size() < 2) {
            continue;
        }
        const uint64_t hash = std::strtoull(f[0].c_str(), nullptr, 16);
        if (f[1] == "P") {
            loaded[hash] = Hint{};
            continue;
        }
        const auto num = [&f](size_t i) -> uint64_t {
            return i < f.size() ? std::strtoull(f[i].c_str(), nullptr, 10) : 0;
        };
        if (f[1] == "I" && f.size() >= 10) {
            HintImage h;
            h.key = num(2);
            h.type = static_cast<uint8_t>(num(3));
            h.num_format = static_cast<uint8_t>(num(4));
            h.data_format = static_cast<uint8_t>(num(5));
            for (size_t k = 0; k < 4; ++k) {
                h.dst_sel[k] = static_cast<uint8_t>(num(6 + k));
            }
            loaded[hash].images.push_back(h);
        } else if (f[1] == "B" && f.size() >= 12) {
            HintBuffer h;
            h.key = num(2);
            h.stride = static_cast<uint16_t>(num(3));
            h.swizzle = static_cast<uint8_t>(num(4));
            h.index_stride = static_cast<uint8_t>(num(5));
            h.element_size = static_cast<uint8_t>(num(6));
            h.num_format = static_cast<uint8_t>(num(7));
            h.data_format = static_cast<uint8_t>(num(8));
            for (size_t k = 0; k < 4; ++k) {
                h.dst_sel[k] = static_cast<uint8_t>(num(9 + k));
            }
            loaded[hash].buffers.push_back(h);
        }
    }
    for (auto& [hash, h] : loaded) {
        g_hints.emplace(hash, std::make_shared<Hint>(std::move(h)));
    }
    std::fprintf(stderr, "SOTC PRETRANSLATE compute hints: %zu programs from %s\n", g_hints.size(), kHintPath);
    std::fflush(stderr);
}

// The sharps of a program's real dispatch: the raw image fields (no post-op: the synthetic sharp gets the same
// post-op at its own fetch), the buffer fields as the spec reads them.
Hint BuildHint(const Shader::Info& info) {
    Hint h;
    const auto* flat = info.FlatBuf().data();
    for (const auto& desc : info.images) {
        AmdGpu::Image raw{};
        if (!desc.Fetch(flat, &raw) || !raw.Valid()) {
            continue;
        }
        HintImage hi;
        hi.key = HintKeyOf(desc.sharp_fetch);
        hi.type = static_cast<uint8_t>(raw.type);
        hi.num_format = static_cast<uint8_t>(raw.num_format);
        hi.data_format = static_cast<uint8_t>(raw.data_format);
        hi.dst_sel = {static_cast<uint8_t>(raw.dst_sel_x), static_cast<uint8_t>(raw.dst_sel_y),
                      static_cast<uint8_t>(raw.dst_sel_z), static_cast<uint8_t>(raw.dst_sel_w)};
        h.images.push_back(hi);
    }
    for (const auto& desc : info.buffers) {
        const AmdGpu::Buffer sharp = desc.GetSharp(info);
        if (!sharp) {
            continue;
        }
        HintBuffer hb;
        hb.key = HintKeyOf(desc.sharp_fetch);
        hb.stride = static_cast<uint16_t>(sharp.stride);
        hb.swizzle = static_cast<uint8_t>(sharp.swizzle_enable);
        hb.index_stride = static_cast<uint8_t>(sharp.index_stride);
        hb.element_size = static_cast<uint8_t>(sharp.element_size);
        hb.num_format = static_cast<uint8_t>(sharp.num_format);
        hb.data_format = static_cast<uint8_t>(sharp.data_format);
        hb.dst_sel = {static_cast<uint8_t>(sharp.dst_sel_x), static_cast<uint8_t>(sharp.dst_sel_y),
                      static_cast<uint8_t>(sharp.dst_sel_z), static_cast<uint8_t>(sharp.dst_sel_w)};
        h.buffers.push_back(hb);
    }
    return h;
}

// sotc: SOTC_PIPE_MANIFEST: BuildHint plus the samplers and the vertex fetch attributes' V#.
Hint BuildFullHintImpl(const Shader::Info& info, const Shader::Gcn::FetchShaderData* fetch) {
    Hint h = BuildHint(info);
    for (const auto& desc : info.samplers) {
        const AmdGpu::Sampler sharp = desc.GetSharp(info);
        HintSampler hs;
        hs.key = HintKeyOf(desc.sharp_fetch);
        hs.force_unnormalized = static_cast<uint8_t>(sharp.force_unnormalized.Value());
        hs.force_degamma = static_cast<uint8_t>(sharp.force_degamma.Value());
        h.samplers.push_back(hs);
    }
    if (fetch && !fetch->Empty()) {
        for (const auto& attrib : fetch->attributes) {
            const uint64_t key = (uint64_t{attrib.sgpr_base} << 8) | attrib.dword_offset;
            if (std::ranges::find(h.attribs, key, &HintBuffer::key) != h.attribs.end()) {
                continue;
            }
            // The raw V# (the attribute's format override is applied by its reader, as at the draw).
            const auto sharp = info.ReadUdReg<AmdGpu::Buffer>(attrib.sgpr_base, attrib.dword_offset);
            HintBuffer hb;
            hb.key = key;
            hb.stride = static_cast<uint16_t>(sharp.stride);
            hb.swizzle = static_cast<uint8_t>(sharp.swizzle_enable);
            hb.index_stride = static_cast<uint8_t>(sharp.index_stride);
            hb.element_size = static_cast<uint8_t>(sharp.element_size);
            hb.num_format = static_cast<uint8_t>(sharp.num_format);
            hb.data_format = static_cast<uint8_t>(sharp.data_format);
            hb.dst_sel = {static_cast<uint8_t>(sharp.dst_sel_x), static_cast<uint8_t>(sharp.dst_sel_y),
                          static_cast<uint8_t>(sharp.dst_sel_z), static_cast<uint8_t>(sharp.dst_sel_w)};
            h.attribs.push_back(hb);
        }
    }
    return h;
}

// Appends a program's hint (a "P" line, then its bindings); the last "P" of a hash wins at the next load.
void AppendHintLocked(uint64_t hash, const Hint& h) {
    if (!g_hint_out) {
        std::error_code ec;
        std::filesystem::create_directories("user/shader_predict", ec);
        g_hint_out = std::fopen(kHintPath, "ab");
        if (!g_hint_out) {
            return;
        }
    }
    std::fprintf(g_hint_out, "%016llx\tP\n", static_cast<unsigned long long>(hash));
    for (const auto& i : h.images) {
        std::fprintf(g_hint_out, "%016llx\tI\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
                     static_cast<unsigned long long>(hash), static_cast<unsigned long long>(i.key), unsigned{i.type},
                     unsigned{i.num_format}, unsigned{i.data_format}, unsigned{i.dst_sel[0]}, unsigned{i.dst_sel[1]},
                     unsigned{i.dst_sel[2]}, unsigned{i.dst_sel[3]});
    }
    for (const auto& b : h.buffers) {
        std::fprintf(g_hint_out, "%016llx\tB\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
                     static_cast<unsigned long long>(hash), static_cast<unsigned long long>(b.key), unsigned{b.stride},
                     unsigned{b.swizzle}, unsigned{b.index_stride}, unsigned{b.element_size}, unsigned{b.num_format},
                     unsigned{b.data_format}, unsigned{b.dst_sel[0]}, unsigned{b.dst_sel[1]}, unsigned{b.dst_sel[2]},
                     unsigned{b.dst_sel[3]});
    }
    std::fflush(g_hint_out);
}

std::string ImageText(const Shader::ImageSpecialization& s) {
    char b[120];
    std::snprintf(b, sizeof b, "t%u i%d s%d sw%d%d%d%d n%u", unsigned(s.type), int{s.is_integer}, int{s.is_storage},
                  int(s.dst_select.r), int(s.dst_select.g), int(s.dst_select.b), int(s.dst_select.a),
                  unsigned{s.num_bindings});
    return b;
}

std::string BufferText(const Shader::BufferSpecialization& s) {
    char b[120];
    std::snprintf(b, sizeof b, "str%u sw%u f%u df%u nf%u sel%d%d%d%d", unsigned{s.stride}, unsigned{s.swizzle_enable},
                  unsigned{s.is_formatted}, unsigned{s.data_format}, unsigned{s.num_format}, int(s.dst_select.r),
                  int(s.dst_select.g), int(s.dst_select.b), int(s.dst_select.a));
    return b;
}

// The bindings whose key parts differ between the prebuilt key and the real one, as text.
void LogMiss(uint64_t hash, bool hinted, const Shader::StageSpecialization& real,
             const Shader::StageSpecialization& predicted) {
    std::string made, got;
    const size_t ni = std::max(real.images.size(), predicted.images.size());
    for (size_t i = 0; i < ni; ++i) {
        if (i < real.images.size() && i < predicted.images.size() && real.images[i] == predicted.images[i]) {
            continue;
        }
        char b[40];
        std::snprintf(b, sizeof b, " img%zu [", i);
        if (i < predicted.images.size()) {
            made += std::string(b) + ImageText(predicted.images[i]) + "]";
        }
        if (i < real.images.size()) {
            got += std::string(b) + ImageText(real.images[i]) + "]";
        }
    }
    const size_t nb = std::max(real.buffers.size(), predicted.buffers.size());
    for (size_t i = 0; i < nb; ++i) {
        if (i < real.buffers.size() && i < predicted.buffers.size() && real.buffers[i] == predicted.buffers[i]) {
            continue;
        }
        char b[40];
        std::snprintf(b, sizeof b, " buf%zu [", i);
        if (i < predicted.buffers.size()) {
            made += std::string(b) + BufferText(predicted.buffers[i]) + "]";
        }
        if (i < real.buffers.size()) {
            got += std::string(b) + BufferText(real.buffers[i]) + "]";
        }
    }
    if (made.empty() && got.empty()) {
        made = got = " (no image or buffer part: other key parts)";
    }
    std::fprintf(stderr, "SOTC PRETRANSLATE compute miss %016llx (hinted %d): made:%s; real:%s\n",
                 static_cast<unsigned long long>(hash), int{hinted}, made.c_str(), got.c_str());
    std::fflush(stderr);
}

} // namespace

bool HintsOn() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PRETRANSLATE_COMPUTE_HINTS");
        return !v || v[0] != '0';
    }();
    return on && ComputeOn();
}

HintPtr AcquireHint(uint64_t hash) noexcept {
    if (!HintsOn()) {
        return {};
    }
    std::scoped_lock lock{g_hint_mutex};
    if (!g_hints_loaded) {
        LoadHintsLocked();
    }
    ++g_hint_lookups;
    auto it = g_hints.find(hash);
    // sotc: a hint an older build wrote under the variant-mixed hash of a SOTC_SHADER_ADDR (plain) or SOTC_HEAP stage
    // (the dispatch's lookup hash, HashCombine(hash, variant)): the sharps are the game's, whatever the variant.
    for (const uint64_t variant : {uint64_t{Shader::StageVariantPlain}, uint64_t{Shader::StageVariantHeap}}) {
        if (it == g_hints.end()) {
            it = g_hints.find(HashCombine(hash, variant));
        }
    }
    if (it == g_hints.end()) {
        return {};
    }
    g_hinted.insert(hash);
    ++g_hint_applied;
    return it->second;
}

Hint BuildFullHint(const Shader::Info& info, const Shader::Gcn::FetchShaderData* fetch) {
    return BuildFullHintImpl(info, fetch);
}

const Hint* ActiveHint() noexcept {
    return t_active_hint;
}

HintScope::HintScope(HintPtr hint) : hint_{std::move(hint)}, prev_{t_active_hint} {
    t_active_hint = hint_.get();
}

HintScope::~HintScope() {
    t_active_hint = prev_;
}

void NoteComputeFirstUse(uint64_t hash, bool hit, const Shader::Info& info, const Shader::StageSpecialization& real,
                         const Shader::StageSpecialization& predicted) noexcept {
    if (!ComputeOn()) {
        return;
    }
    // sotc: the game's hash, as AcquireHint is asked for it (the prebuild's program hash). `hash` is the dispatch's
    // lookup hash, the variant-mixed one under SOTC_SHADER_ADDR / SOTC_HEAP; the miss log keeps showing it.
    const uint64_t game_hash = info.pgm_hash;
    std::scoped_lock lock{g_hint_mutex};
    const bool hinted = g_hinted.erase(game_hash) != 0;
    if (hinted) {
        ++g_hint_first;
        g_hint_first_hit += hit;
    }
    if (hit) {
        return;
    }
    if (g_miss_logged < 32) {
        ++g_miss_logged;
        LogMiss(hash, hinted, real, predicted);
    }
    if (!HintsOn()) {
        return;
    }
    if (!g_hints_loaded) {
        LoadHintsLocked();
    }
    Hint fresh = BuildHint(info);
    const auto it = g_hints.find(game_hash);
    if (it != g_hints.end() && *it->second == fresh) {
        return;
    }
    if (it == g_hints.end()) {
        ++g_hint_new;
    }
    AppendHintLocked(game_hash, fresh);
    ++g_hint_written;
    g_hints[game_hash] = std::make_shared<Hint>(std::move(fresh));
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
    const auto print_diff = [&](char* line, int& n, size_t size, const Counters& c) {
        for (int b = 0; b < DiffBits && n > 0 && n < static_cast<int>(size) - 32; ++b) {
            if (const uint64_t v = c.diff[b].load()) {
                n += std::snprintf(line + n, size - n, " %s %llu", kDiff[b], (unsigned long long)v);
            }
        }
    };
    for (int s = 0; s < 2 && PixelVertexOn(); ++s) {
        const auto& c = g_counters[s];
        char line[1792];
        int n = std::snprintf(
            line, sizeof line,
            "SOTC PRETRANSLATE %s: queued %llu (known %llu, unpaired %llu, off with SOTC_SHADER_ADDR %llu, other %llu), "
            "built %llu, failed %llu, superseded %llu; first draws %llu, key hit %llu; misses by key part:",
            kStage[s], (unsigned long long)c.queued.load(), (unsigned long long)c.known.load(),
            (unsigned long long)c.unpaired.load(), (unsigned long long)c.mode.load(),
            (unsigned long long)c.other.load(), (unsigned long long)c.built.load(),
            (unsigned long long)c.failed.load(), (unsigned long long)c.superseded.load(),
            (unsigned long long)c.used.load(), (unsigned long long)c.hits.load());
        print_diff(line, n, sizeof line, c);
        out += line;
        out += '\n';
    }
    if (!ComputeOn()) {
        return;
    }
    const auto& c = g_counters[2];
    char line[1792];
    int n = std::snprintf(
        line, sizeof line,
        "SOTC PRETRANSLATE compute: queued %llu, built %llu, failed %llu, known %llu, off with SOTC_SHADER_ADDR %llu, "
        "other %llu, first dispatches %llu, key hit %llu; misses by key part:",
        (unsigned long long)c.queued.load(), (unsigned long long)c.built.load(), (unsigned long long)c.failed.load(),
        (unsigned long long)c.known.load(), (unsigned long long)c.mode.load(), (unsigned long long)c.other.load(),
        (unsigned long long)c.used.load(), (unsigned long long)c.hits.load());
    print_diff(line, n, sizeof line, c);
    if (n > 0 && n < static_cast<int>(sizeof line) - 160) {
        n += std::snprintf(line + n, sizeof line - n,
                           "; layouts lo-hi-rsrc1 %llu, lo-rsrc1 %llu, none %llu (both valid %llu); verify match %llu, "
                           "mismatch %llu",
                           (unsigned long long)c.layout[1].load(), (unsigned long long)c.layout[2].load(),
                           (unsigned long long)c.layout[0].load(), (unsigned long long)c.ambiguous.load(),
                           (unsigned long long)c.verify_match.load(), (unsigned long long)c.verify_mismatch.load());
    }
    out += line;
    out += '\n';
    // sotc: SOTC_PRETRANSLATE_COMPUTE_HINTS counters (one line; the hint file is user/shader_predict/compute_hints.tsv).
    std::scoped_lock hint_lock{g_hint_mutex};
    char hl[560];
    std::snprintf(hl, sizeof hl,
                  "SOTC PRETRANSLATE compute hints (%s): file %zu programs; prebuilt with a hint %llu, their first "
                  "dispatches %llu (key hit %llu); misses logged %llu, hint lines written %llu (new programs %llu); "
                  "lookups %llu\n",
                  HintsOn() ? "on" : "off", g_hints.size(), (unsigned long long)g_hint_applied,
                  (unsigned long long)g_hint_first, (unsigned long long)g_hint_first_hit,
                  (unsigned long long)g_miss_logged, (unsigned long long)g_hint_written,
                  (unsigned long long)g_hint_new, (unsigned long long)g_hint_lookups);
    out += hl;
}

} // namespace SotcPretranslate
