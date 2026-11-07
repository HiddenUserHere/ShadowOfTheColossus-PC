// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PIPE_MANIFEST, see pipe_manifest.h.

#include "pipe_manifest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace SotcPipeManifest {
namespace {

int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string UserDir() {
    const char* dir = std::getenv("BB_GPU_USER_DIR");
    return (dir && *dir) ? std::string{dir} : std::string{"user"};
}

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}
constexpr uint64_t kFnvSeed = 0xcbf29ce484222325ull;

std::string Hex(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

bool Unhex(const std::string& text, std::vector<uint8_t>& out) {
    if (text.size() % 2 != 0) {
        return false;
    }
    const auto nibble = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    out.resize(text.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

std::vector<std::string> Split(const std::string& line, char sep) {
    std::vector<std::string> f;
    std::string item;
    std::istringstream in(line);
    while (std::getline(in, item, sep)) {
        f.push_back(item);
    }
    return f;
}

// Capture state (command processor).
std::mutex g_cap_mutex;
std::unordered_set<uint64_t> g_known;
std::string g_pending;     // lines not written yet
bool g_header_written = false;
std::atomic<uint64_t> g_appended{0};

// Skip list.
std::mutex g_skip_mutex;
std::unordered_map<uint64_t, uint8_t> g_skips; // stage id -> bit mask of SkipKind
std::string g_skip_variant;

// Code store (loader thread writes, command processor reads).
std::mutex g_code_mutex;
bool g_wanted_set = false;
std::unordered_set<uint64_t> g_wanted;
std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint32_t>>> g_code;
size_t g_code_bytes = 0;
constexpr size_t kUnfilteredCap = size_t{256} << 20; // before SetWanted: every program, up to this
std::atomic<uint64_t> g_arrivals{0};
std::atomic<int64_t> g_last_program_ns{0};

std::string HeaderLine(const Header& h) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "H\tformat=1\tgame=%s\tkey=%u\trt=%u\tmeta=%u\tvariant=", h.game.c_str(),
                  h.key_size, h.rt_size, h.meta_version);
    return std::string{buf} + h.variant + "\n";
}

void HintBufferText(std::string& out, char tag, const SotcPretranslate::HintBuffer& b) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%c,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u;", tag, static_cast<unsigned long long>(b.key),
                  unsigned{b.stride}, unsigned{b.swizzle}, unsigned{b.index_stride}, unsigned{b.element_size},
                  unsigned{b.num_format}, unsigned{b.data_format}, unsigned{b.dst_sel[0]}, unsigned{b.dst_sel[1]},
                  unsigned{b.dst_sel[2]}, unsigned{b.dst_sel[3]});
    out += buf;
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PIPE_MANIFEST");
        return !v || v[0] != '0';
    }();
    return on;
}

std::string ShippedPath() {
    const char* dir = std::getenv("SOTC_PIPE_MANIFEST_DIR");
    const std::string base = (dir && *dir) ? std::string{dir} : std::string{"data/pipeline_manifest"};
    return base + "/sotc-pipelines.tsv";
}

std::string UserPath() {
    return UserDir() + "/shader_predict/sotc-pipelines.user.tsv";
}

std::string HintText(const SotcPretranslate::Hint& hint) {
    std::string out;
    for (const auto& i : hint.images) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "I,%llu,%u,%u,%u,%u,%u,%u,%u;", static_cast<unsigned long long>(i.key),
                      unsigned{i.type}, unsigned{i.num_format}, unsigned{i.data_format}, unsigned{i.dst_sel[0]},
                      unsigned{i.dst_sel[1]}, unsigned{i.dst_sel[2]}, unsigned{i.dst_sel[3]});
        out += buf;
    }
    for (const auto& b : hint.buffers) {
        HintBufferText(out, 'B', b);
    }
    for (const auto& a : hint.attribs) {
        HintBufferText(out, 'A', a);
    }
    for (const auto& s : hint.samplers) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "S,%llu,%u,%u;", static_cast<unsigned long long>(s.key),
                      unsigned{s.force_unnormalized}, unsigned{s.force_degamma});
        out += buf;
    }
    if (out.empty()) {
        return "-";
    }
    out.pop_back(); // the last ';'
    return out;
}

bool ParseHint(const std::string& text, SotcPretranslate::Hint& out) {
    out = {};
    if (text == "-" || text.empty()) {
        return true;
    }
    for (const auto& item : Split(text, ';')) {
        const auto f = Split(item, ',');
        if (f.empty() || f[0].size() != 1) {
            return false;
        }
        std::vector<uint64_t> n;
        for (size_t i = 1; i < f.size(); ++i) {
            n.push_back(std::strtoull(f[i].c_str(), nullptr, 10));
        }
        const char tag = f[0][0];
        if (tag == 'I' && n.size() == 8) {
            SotcPretranslate::HintImage h;
            h.key = n[0];
            h.type = static_cast<uint8_t>(n[1]);
            h.num_format = static_cast<uint8_t>(n[2]);
            h.data_format = static_cast<uint8_t>(n[3]);
            for (size_t k = 0; k < 4; ++k) {
                h.dst_sel[k] = static_cast<uint8_t>(n[4 + k]);
            }
            out.images.push_back(h);
        } else if ((tag == 'B' || tag == 'A') && n.size() == 11) {
            SotcPretranslate::HintBuffer h;
            h.key = n[0];
            h.stride = static_cast<uint16_t>(n[1]);
            h.swizzle = static_cast<uint8_t>(n[2]);
            h.index_stride = static_cast<uint8_t>(n[3]);
            h.element_size = static_cast<uint8_t>(n[4]);
            h.num_format = static_cast<uint8_t>(n[5]);
            h.data_format = static_cast<uint8_t>(n[6]);
            for (size_t k = 0; k < 4; ++k) {
                h.dst_sel[k] = static_cast<uint8_t>(n[7 + k]);
            }
            (tag == 'B' ? out.buffers : out.attribs).push_back(h);
        } else if (tag == 'S' && n.size() == 3) {
            SotcPretranslate::HintSampler h;
            h.key = n[0];
            h.force_unnormalized = static_cast<uint8_t>(n[1]);
            h.force_degamma = static_cast<uint8_t>(n[2]);
            out.samplers.push_back(h);
        } else {
            return false;
        }
    }
    return true;
}

uint64_t StageId(uint64_t hash, uint8_t variant, const std::vector<uint8_t>& spec, const SotcPretranslate::Hint& hint) {
    uint64_t h = Fnv(kFnvSeed, &hash, sizeof hash);
    h = Fnv(h, &variant, sizeof variant);
    h = Fnv(h, spec.data(), spec.size());
    const std::string text = HintText(hint);
    return Fnv(h, text.data(), text.size());
}

uint64_t PipeId(bool compute, const std::vector<uint8_t>& key, const std::vector<std::pair<uint8_t, uint64_t>>& stages) {
    const uint8_t kind = compute ? 1 : 0;
    uint64_t h = Fnv(kFnvSeed, &kind, sizeof kind);
    h = Fnv(h, key.data(), key.size());
    for (const auto& [slot, id] : stages) {
        h = Fnv(h, &slot, sizeof slot);
        h = Fnv(h, &id, sizeof id);
    }
    return h;
}

namespace {

// One file: its records under accepted headers go to `stages` / `pipes` (by id; the first record of an id wins).
size_t ReadFile(const std::string& path, const Header& want, std::unordered_map<uint64_t, Stage>& stages,
                std::unordered_map<uint64_t, Pipe>& pipes, std::vector<uint64_t>& pipe_order, Data& data,
                std::string& log) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log += "SOTC PIPE MANIFEST: " + path + ": none\n";
        return 0;
    }
    bool accepted = false;
    bool seen_header = false;
    size_t read_pipes = 0, read_stages = 0, refused = 0, bad = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto f = Split(line, '\t');
        if (f[0] == "H") {
            seen_header = true;
            Header h;
            uint32_t format = 0;
            for (size_t i = 1; i < f.size(); ++i) {
                const auto eq = f[i].find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const std::string name = f[i].substr(0, eq), value = f[i].substr(eq + 1);
                if (name == "format") {
                    format = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                } else if (name == "key") {
                    h.key_size = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                } else if (name == "rt") {
                    h.rt_size = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                } else if (name == "meta") {
                    h.meta_version = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                } else if (name == "variant") {
                    h.variant = value;
                }
            }
            accepted = format == 1 && h.key_size == want.key_size && h.rt_size == want.rt_size &&
                       h.meta_version == want.meta_version;
            if (accepted && h.variant != want.variant) {
                data.variant_differs = true;
            }
            continue;
        }
        if (!seen_header || !accepted) {
            ++refused;
            continue;
        }
        if (f[0] == "S" && f.size() >= 6) {
            Stage s;
            s.id = std::strtoull(f[1].c_str(), nullptr, 16);
            s.hash = std::strtoull(f[2].c_str(), nullptr, 16);
            s.variant = static_cast<uint8_t>(std::strtoul(f[3].c_str(), nullptr, 10));
            if (!Unhex(f[4], s.spec) || s.spec.empty() || !ParseHint(f[5], s.hint)) {
                ++bad;
                continue;
            }
            ++read_stages;
            stages.try_emplace(s.id, std::move(s));
        } else if ((f[0] == "G" && f.size() >= 4) || (f[0] == "C" && f.size() >= 3)) {
            Pipe p;
            p.id = std::strtoull(f[1].c_str(), nullptr, 16);
            p.compute = f[0] == "C";
            bool ok = true;
            if (p.compute) {
                p.stages.emplace_back(uint8_t{0}, std::strtoull(f[2].c_str(), nullptr, 16));
            } else {
                ok = Unhex(f[2], p.key) && p.key.size() == want.key_size;
                for (const auto& item : Split(f[3], ',')) {
                    const auto colon = item.find(':');
                    if (colon == std::string::npos) {
                        ok = false;
                        break;
                    }
                    p.stages.emplace_back(static_cast<uint8_t>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 10)),
                                          std::strtoull(item.substr(colon + 1).c_str(), nullptr, 16));
                }
            }
            if (!ok || p.stages.empty()) {
                ++bad;
                continue;
            }
            ++read_pipes;
            if (pipes.try_emplace(p.id, p).second) {
                pipe_order.push_back(p.id);
            }
        } else {
            ++bad;
        }
    }
    data.refused_records += refused;
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "SOTC PIPE MANIFEST: %s: %zu pipelines, %zu stages (records of another layout %zu, unreadable %zu)\n",
                  path.c_str(), read_pipes, read_stages, refused, bad);
    log += buf;
    return read_pipes;
}

} // namespace

Data Load(const Header& want, std::string& log) {
    Data data;
    std::unordered_map<uint64_t, Stage> stages;
    std::unordered_map<uint64_t, Pipe> pipes;
    std::vector<uint64_t> order;
    data.shipped_pipes = ReadFile(ShippedPath(), want, stages, pipes, order, data, log);
    data.user_pipes = ReadFile(UserPath(), want, stages, pipes, order, data, log);
    std::unordered_set<uint64_t> used;
    {
        std::scoped_lock lock{g_cap_mutex};
        for (const auto& [id, s] : stages) {
            g_known.insert(id);
        }
        for (const uint64_t id : order) {
            g_known.insert(id);
        }
    }
    for (const uint64_t id : order) {
        Pipe& p = pipes[id];
        const bool complete = std::ranges::all_of(p.stages, [&](const auto& s) { return stages.contains(s.second); });
        if (!complete) {
            continue;
        }
        for (const auto& s : p.stages) {
            used.insert(s.second);
        }
        data.pipes.push_back(std::move(p));
    }
    for (auto& [id, s] : stages) {
        if (used.contains(id)) {
            data.stages.push_back(std::move(s));
        }
    }
    // A fixed order (the files' order is not): stages by program, so a program's permutations are made together.
    std::ranges::sort(data.stages, [](const Stage& a, const Stage& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.id < b.id;
    });
    return data;
}

bool Known(uint64_t id) {
    std::scoped_lock lock{g_cap_mutex};
    return g_known.contains(id);
}

void AppendStage(const Header& header, const Stage& stage) {
    std::scoped_lock lock{g_cap_mutex};
    if (!g_known.insert(stage.id).second) {
        return;
    }
    if (!g_header_written) {
        g_header_written = true;
        g_pending += HeaderLine(header);
    }
    char head[96];
    std::snprintf(head, sizeof head, "S\t%016llx\t%016llx\t%u\t", static_cast<unsigned long long>(stage.id),
                  static_cast<unsigned long long>(stage.hash), unsigned{stage.variant});
    g_pending += head;
    g_pending += Hex(stage.spec);
    g_pending += '\t';
    g_pending += HintText(stage.hint);
    g_pending += '\n';
    g_appended.fetch_add(1, std::memory_order_relaxed);
}

void AppendPipe(const Header& header, const Pipe& pipe) {
    std::scoped_lock lock{g_cap_mutex};
    if (!g_known.insert(pipe.id).second) {
        return;
    }
    if (!g_header_written) {
        g_header_written = true;
        g_pending += HeaderLine(header);
    }
    char head[64];
    std::snprintf(head, sizeof head, "%c\t%016llx\t", pipe.compute ? 'C' : 'G', static_cast<unsigned long long>(pipe.id));
    g_pending += head;
    if (pipe.compute) {
        char id[24];
        std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(pipe.stages.at(0).second));
        g_pending += id;
    } else {
        g_pending += Hex(pipe.key);
        g_pending += '\t';
        for (size_t i = 0; i < pipe.stages.size(); ++i) {
            char item[40];
            std::snprintf(item, sizeof item, "%s%u:%016llx", i ? "," : "", unsigned{pipe.stages[i].first},
                          static_cast<unsigned long long>(pipe.stages[i].second));
            g_pending += item;
        }
    }
    g_pending += '\n';
    g_appended.fetch_add(1, std::memory_order_relaxed);
}

void Flush() {
    std::string out;
    {
        std::scoped_lock lock{g_cap_mutex};
        out.swap(g_pending);
    }
    if (out.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{UserPath()}.parent_path(), ec);
    if (std::FILE* f = std::fopen(UserPath().c_str(), "ab")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}

uint64_t ProcessPrivateBytes() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return counters.PrivateUsage;
    }
#endif
    return 0;
}

uint64_t Appended() noexcept {
    return g_appended.load(std::memory_order_relaxed);
}

namespace {
std::string SkipPath() {
    return UserDir() + "/shader_predict/sotc-pipelines.skip.tsv";
}
uint64_t VariantHash(const std::string& variant) {
    return Fnv(kFnvSeed, variant.data(), variant.size());
}
} // namespace

void LoadSkips(const std::string& variant) {
    std::scoped_lock lock{g_skip_mutex};
    g_skip_variant = variant;
    g_skips.clear();
    std::ifstream in(SkipPath(), std::ios::binary);
    const uint64_t vh = VariantHash(variant);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto f = Split(line, '\t');
        if (f.size() < 3 || std::strtoull(f[0].c_str(), nullptr, 16) != vh) {
            continue;
        }
        const uint64_t id = std::strtoull(f[1].c_str(), nullptr, 16);
        const unsigned kind = static_cast<unsigned>(std::strtoul(f[2].c_str(), nullptr, 10));
        if (kind <= 1) {
            g_skips[id] |= static_cast<uint8_t>(1u << kind);
        }
    }
}

bool Skipped(uint64_t stage_id, SkipKind kind) {
    std::scoped_lock lock{g_skip_mutex};
    const auto it = g_skips.find(stage_id);
    return it != g_skips.end() && (it->second >> static_cast<unsigned>(kind) & 1u) != 0;
}

void NoteSkip(uint64_t stage_id, SkipKind kind, const char* reason) {
    std::scoped_lock lock{g_skip_mutex};
    auto& mask = g_skips[stage_id];
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<unsigned>(kind));
    if (mask & bit) {
        return;
    }
    mask |= bit;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{SkipPath()}.parent_path(), ec);
    if (std::FILE* f = std::fopen(SkipPath().c_str(), "ab")) {
        std::fprintf(f, "%016llx\t%016llx\t%u\t%s\n", static_cast<unsigned long long>(VariantHash(g_skip_variant)),
                     static_cast<unsigned long long>(stage_id), static_cast<unsigned>(kind), reason);
        std::fclose(f);
    }
}

void NoteProgram(uint64_t hash, const void* code, size_t bytes) noexcept {
    if (!On()) {
        return;
    }
    g_last_program_ns.store(SteadyNs(), std::memory_order_relaxed);
    if (!code || bytes < 4) {
        return;
    }
    std::scoped_lock lock{g_code_mutex};
    if (g_wanted_set ? !g_wanted.contains(hash) : g_code_bytes + bytes > kUnfilteredCap) {
        return;
    }
    if (g_code.contains(hash)) {
        return;
    }
    auto words = std::make_shared<std::vector<uint32_t>>(bytes / 4);
    std::memcpy(words->data(), code, words->size() * 4);
    g_code_bytes += bytes;
    g_code.emplace(hash, std::move(words));
    if (g_wanted_set) {
        g_arrivals.fetch_add(1, std::memory_order_relaxed);
    }
}

void SetWanted(const std::vector<uint64_t>& hashes) {
    std::scoped_lock lock{g_code_mutex};
    g_wanted.clear();
    g_wanted.insert(hashes.begin(), hashes.end());
    g_wanted_set = true;
    uint64_t kept = 0;
    for (auto it = g_code.begin(); it != g_code.end();) {
        if (g_wanted.contains(it->first)) {
            ++kept;
            ++it;
        } else {
            g_code_bytes -= it->second->size() * 4;
            it = g_code.erase(it);
        }
    }
    g_arrivals.fetch_add(kept, std::memory_order_relaxed);
}

std::shared_ptr<const std::vector<uint32_t>> Code(uint64_t hash) {
    std::scoped_lock lock{g_code_mutex};
    const auto it = g_code.find(hash);
    return it != g_code.end() ? it->second : nullptr;
}

uint64_t CodeArrivals() noexcept {
    return g_arrivals.load(std::memory_order_relaxed);
}

int64_t LastProgramNs() noexcept {
    return g_last_program_ns.load(std::memory_order_relaxed);
}

void DropCode() {
    std::scoped_lock lock{g_code_mutex};
    g_code.clear();
    g_code_bytes = 0;
}

} // namespace SotcPipeManifest
