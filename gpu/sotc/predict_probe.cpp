// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PREDICT_PROBE=1 (off by default; measurement only, nothing is changed). Step 1 of translating the game's
// shaders at area load instead of at their first draw: for every new translation it measures how much of the
// translation state was knowable before the draw.
//
//   - known at load: the engine's resource loader decompressed the program before this translation (the
//     RES_DECOMPRESS_DONE site of probe_csdr.cpp, which needs SOTC_CSDR_PROBE=1 for the hook; with the predict probe
//     on, that site only records hashes and writes no TSV unless SOTC_CSDR_PROBE_OUT is set), and how long before;
//   - in table: the program is in SOTC_PREDICT_TABLE (tab-separated: hash, footer type, pass, file count; written
//     offline from the game's shader files), which gives its pass;
//   - pixel layout: the color targets, MRTZ mask and sample count equal the first ones seen for the same pass
//     (the pass decides the render targets);
//   - resources off the default: images that are not Color2D, integer, sRGB or storage; formatted buffers whose
//     format is not 32-bit float. The default is what a translation at load would assume without the descriptors.
// One line per stage every 5 s: "SOTC PREDICT <stage>: ...".

#include "predict_probe.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "shader_recompiler/info.h"
#include "shader_recompiler/specialization.h"

namespace SotcPredict {
namespace {

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct TableEntry {
    uint8_t type{};
    std::string pass;
};

struct FsLayout {
    std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers> color{};
    uint8_t mrtz_mask{};
    uint8_t num_samples{};
    bool operator==(const FsLayout& o) const {
        return color == o.color && mrtz_mask == o.mrtz_mask && num_samples == o.num_samples;
    }
};

struct StageCounters {
    uint64_t translations{}, permutations{}, known{}, in_table{}, lead_ms_sum{}, lead_ms_max{};
    uint64_t layout_first{}, layout_match{}, layout_miss{};
    uint64_t images{}, img_not_2d{}, img_integer{}, img_srgb{}, img_storage{};
    uint64_t buffers{}, buf_formatted{}, buf_other_format{};
};

constexpr int kStages = 4; // pixel, vertex, compute, other

std::mutex g_mutex;
std::unordered_map<uint64_t, uint64_t> g_loaded; // program hash -> first load time (ns)
std::unordered_map<uint64_t, TableEntry> g_table;
std::unordered_map<std::string, FsLayout> g_pass_layout;
std::array<StageCounters, kStages> g_counters{};
bool g_table_loaded = false;

void LoadTable() {
    g_table_loaded = true;
    const char* path = std::getenv("SOTC_PREDICT_TABLE");
    if (!path || !*path) {
        return;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string hash, type, pass;
        if (!std::getline(fields, hash, '\t') || !std::getline(fields, type, '\t') ||
            !std::getline(fields, pass, '\t')) {
            continue;
        }
        g_table[std::strtoull(hash.c_str(), nullptr, 16)] = {static_cast<uint8_t>(std::atoi(type.c_str())), pass};
    }
    std::fprintf(stderr, "SOTC PREDICT: %zu programs in %s\n", g_table.size(), path);
}

int StageIndex(Shader::SwStage stage) {
    switch (stage) {
    case Shader::SwStage::Fragment:
        return 0;
    case Shader::SwStage::Vertex:
        return 1;
    case Shader::SwStage::Compute:
        return 2;
    default:
        return 3;
    }
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PREDICT_PROBE");
        return v && v[0] == '1';
    }();
    return on;
}

void NoteLoaded(uint64_t program_hash) noexcept {
    if (!On()) {
        return;
    }
    const uint64_t now = NowNs();
    std::scoped_lock lock{g_mutex};
    g_loaded.try_emplace(program_hash, now);
}

void OnTranslate(const Shader::Info& info, const Shader::StageSpecialization& spec, size_t perm_idx) noexcept {
    if (!On()) {
        return;
    }
    const uint64_t now = NowNs();
    std::scoped_lock lock{g_mutex};
    if (!g_table_loaded) {
        LoadTable();
    }
    StageCounters& c = g_counters[StageIndex(info.sw_stage)];
    ++c.translations;
    c.permutations += perm_idx != 0;
    if (const auto it = g_loaded.find(info.pgm_hash); it != g_loaded.end()) {
        ++c.known;
        const uint64_t lead_ms = (now - it->second) / 1'000'000;
        c.lead_ms_sum += lead_ms;
        c.lead_ms_max = lead_ms > c.lead_ms_max ? lead_ms : c.lead_ms_max;
    }
    const auto entry = g_table.find(info.pgm_hash);
    if (entry != g_table.end()) {
        ++c.in_table;
    }
    if (info.sw_stage == Shader::SwStage::Fragment && entry != g_table.end()) {
        FsLayout layout;
        layout.color = spec.runtime_info.hw.fs.color_buffers;
        layout.mrtz_mask = spec.runtime_info.hw.fs.mrtz_mask;
        layout.num_samples = spec.runtime_info.hw.fs.num_samples;
        const auto [it, first] = g_pass_layout.try_emplace(entry->second.pass, layout);
        if (first) {
            ++c.layout_first;
        } else if (it->second == layout) {
            ++c.layout_match;
        } else {
            ++c.layout_miss;
        }
    }
    for (const auto& image : spec.images) {
        ++c.images;
        c.img_not_2d += image.type != AmdGpu::ImageType::Color2D;
        c.img_integer += image.is_integer;
        c.img_srgb += image.is_srgb;
        c.img_storage += image.is_storage;
    }
    for (const auto& buffer : spec.buffers) {
        ++c.buffers;
        if (buffer.is_formatted) {
            ++c.buf_formatted;
            c.buf_other_format += !(buffer.data_format == static_cast<u32>(AmdGpu::DataFormat::Format32) &&
                                    buffer.num_format == static_cast<u32>(AmdGpu::NumberFormat::Float));
        }
    }
}

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    static constexpr const char* kNames[kStages] = {"pixel", "vertex", "compute", "other"};
    std::scoped_lock lock{g_mutex};
    for (int s = 0; s < kStages; ++s) {
        const StageCounters& c = g_counters[s];
        if (c.translations == 0) {
            continue;
        }
        char line[640];
        std::snprintf(line, sizeof line,
                      "SOTC PREDICT %s (session, %.0f s window): translations %llu (new permutations %llu); known at "
                      "load %llu (lead avg %llu ms, max %llu ms); in table %llu; pass layout first %llu / match "
                      "%llu / miss %llu; images %llu (not 2D %llu, integer %llu, sRGB %llu, storage %llu); buffers "
                      "%llu (formatted %llu, not 32-bit float %llu); loaded programs %zu\n",
                      kNames[s], seconds, (unsigned long long)c.translations, (unsigned long long)c.permutations,
                      (unsigned long long)c.known, (unsigned long long)(c.known ? c.lead_ms_sum / c.known : 0),
                      (unsigned long long)c.lead_ms_max, (unsigned long long)c.in_table,
                      (unsigned long long)c.layout_first, (unsigned long long)c.layout_match,
                      (unsigned long long)c.layout_miss, (unsigned long long)c.images,
                      (unsigned long long)c.img_not_2d, (unsigned long long)c.img_integer,
                      (unsigned long long)c.img_srgb, (unsigned long long)c.img_storage,
                      (unsigned long long)c.buffers, (unsigned long long)c.buf_formatted,
                      (unsigned long long)c.buf_other_format, g_loaded.size());
        out += line;
    }
}

} // namespace SotcPredict
