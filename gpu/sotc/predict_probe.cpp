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
//   - pixel layout: the color targets, MRTZ mask and sample count are one of the layouts known for the program's
//     pass. The set per pass is seeded by every cached pixel shader the warm-up loads (OnCachedProgram), grows with
//     each layout met in play, and persists in SOTC_PREDICT_LAYOUTS (one line per pass and layout), so a layout seen
//     once is covered from the next session on. A translation at load would build one module per layout of the set;
//     the report gives the average set size per translation (the cost of that);
//   - resources off the default: images that are not Color2D, integer, sRGB or storage; formatted buffers whose
//     format is not 32-bit float. The default is what a translation at load would assume without the descriptors;
//   - pixel registers (step 2a): when the loader hands a resource on (RES_LOADED), every pixel program in it gets a
//     predicted register state from the shader file itself: the program descriptor (dword 0x2DEC0400, then the hash
//     low 32 bits) holds SPI_SHADER_PGM_RSRC1 at +17 dwords, RSRC2 at +18, SPI_PS_INPUT_ENA at +21, SPI_PS_INPUT_ADDR
//     at +22 and the interpolant count at +23; the inputs come from the code (v_interp attribute indices: count =
//     highest + 1, flat = read with v_interp_mov only, param = slot, flat and unread inputs passthrough, as the key
//     keys them after SOTC_PS_INPUT_NORMALIZE). At the
//     first translation each field is compared with the draw's real RuntimeInfo.
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
#include <vector>

#include "shader_recompiler/frontend/decode.h"
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
    uint64_t layout_hit{}, layout_miss{}, layout_set_sum{};
    uint64_t rule_hit{}, rule_miss{}, rule_cands_sum{}, union_hit{};
    // step 2a, pixel: register and input prediction from the shader file, per field.
    uint64_t pred_found{}, pred_user{}, pred_vgprs{}, pred_fp{}, pred_ena{}, pred_addr{}, pred_ninputs{},
        pred_ninterp{}, pred_inputs{}, pred_all{};
    uint64_t images{}, img_not_2d{}, img_integer{}, img_srgb{}, img_storage{};
    uint64_t buffers{}, buf_formatted{}, buf_other_format{};
};

constexpr int kStages = 4; // pixel, vertex, compute, other

std::mutex g_mutex;
std::unordered_map<uint64_t, uint64_t> g_loaded; // program hash -> first load time (ns)
std::unordered_map<uint64_t, TableEntry> g_table;
std::unordered_map<std::string, std::vector<FsLayout>> g_pass_layouts;
std::FILE* g_layouts_out = nullptr;
size_t g_layout_count = 0;
std::array<StageCounters, kStages> g_counters{};
bool g_table_loaded = false;

// Step 2a: the register state a pixel program declares in its shader file, and its reads from the code.
struct PsPrediction {
    uint32_t rsrc1{}, rsrc2{}, ena{}, addr{}, ninterp{};
    uint64_t read_mask{};     // attributes read by any v_interp
    uint64_t interp_mask{};   // attributes read by v_interp_p1/p2 (interpolated)
    std::array<uint8_t, 32> param{}; // the VS export param of each input slot (identity unless the tail says else)
};
std::unordered_map<uint64_t, PsPrediction> g_ps_pred;

uint32_t Dword(const unsigned char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// The pixel program whose footer is at data + footer: its code (marker 0xBEEB03FF, imm, footer at marker +
// (imm + 1) * 8) and its descriptor (0x2DEC0400 + hash low 32, nearest before the marker). False when not found.
bool PredictPixel(const unsigned char* data, size_t footer, uint64_t hash, uint32_t length, PsPrediction& out) {
    size_t marker = SIZE_MAX;
    for (size_t m = footer & ~size_t{3}; m >= 8 && footer - m < (size_t{1} << 20);) {
        m -= 4;
        if (Dword(data + m) == 0xBEEB03FFu && m + (size_t{Dword(data + m + 4)} + 1) * 8 == footer) {
            marker = m;
            break;
        }
    }
    if (marker == SIZE_MAX || marker + length > footer) {
        return false;
    }
    size_t desc = SIZE_MAX;
    for (size_t d = marker; d >= 4 && marker - d < 64 * 4;) {
        d -= 4;
        if (Dword(data + d) == 0x2DEC0400u && d + 4 < marker && Dword(data + d + 4) == static_cast<uint32_t>(hash)) {
            desc = d;
            break;
        }
    }
    if (desc == SIZE_MAX || desc + 24 * 4 > marker) {
        return false;
    }
    out.rsrc1 = Dword(data + desc + 17 * 4);
    out.rsrc2 = Dword(data + desc + 18 * 4);
    out.ena = Dword(data + desc + 21 * 4);
    out.addr = Dword(data + desc + 22 * 4);
    out.ninterp = Dword(data + desc + 23 * 4);
    for (size_t i = 0; i < out.param.size(); ++i) {
        out.param[i] = static_cast<uint8_t>(i);
    }
    const auto* code = reinterpret_cast<const u32*>(data + marker);
    Shader::Gcn::GcnCodeSlice slice(code, code + length / 4);
    Shader::Gcn::GcnDecodeContext decoder;
    while (!slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(slice);
        if (inst.opcode == Shader::Gcn::Opcode::V_INTERP_P1_F32 ||
            inst.opcode == Shader::Gcn::Opcode::V_INTERP_P2_F32) {
            out.read_mask |= uint64_t{1} << inst.control.vintrp.attr;
            out.interp_mask |= uint64_t{1} << inst.control.vintrp.attr;
        } else if (inst.opcode == Shader::Gcn::Opcode::V_INTERP_MOV_F32) {
            out.read_mask |= uint64_t{1} << inst.control.vintrp.attr;
        } else if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
            break;
        }
    }
    return true;
}

void LoadLayouts();

void LoadTable() {
    g_table_loaded = true;
    LoadLayouts();
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

constexpr int kLayoutBytes = static_cast<int>(sizeof(FsLayout));

// Adds a layout to its pass's set; true when it was new (then it is appended to the layouts file).
bool AddLayout(const std::string& pass, const FsLayout& layout, bool persist) {
    auto& set = g_pass_layouts[pass];
    for (const FsLayout& known : set) {
        if (known == layout) {
            return false;
        }
    }
    set.push_back(layout);
    ++g_layout_count;
    if (persist && g_layouts_out) {
        std::string hex;
        const auto* bytes = reinterpret_cast<const unsigned char*>(&layout);
        char b[4];
        for (int i = 0; i < kLayoutBytes; ++i) {
            std::snprintf(b, sizeof b, "%02x", bytes[i]);
            hex += b;
        }
        std::fprintf(g_layouts_out, "%s\t%s\n", pass.c_str(), hex.c_str());
        std::fflush(g_layouts_out);
    }
    return true;
}

// SOTC_PREDICT_LAYOUTS: "FSLAYOUT <bytes>" then "pass<TAB>hex" lines; a file of another layout size is ignored.
void LoadLayouts() {
    const char* path = std::getenv("SOTC_PREDICT_LAYOUTS");
    if (!path || !*path) {
        return;
    }
    size_t loaded = 0;
    bool header_ok = false;
    {
        std::ifstream in(path);
        std::string line;
        if (std::getline(in, line)) {
            header_ok = line == "FSLAYOUT " + std::to_string(kLayoutBytes);
        }
        while (header_ok && std::getline(in, line)) {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos || line.size() - tab - 1 != static_cast<size_t>(kLayoutBytes) * 2) {
                continue;
            }
            FsLayout layout;
            auto* bytes = reinterpret_cast<unsigned char*>(&layout);
            for (int i = 0; i < kLayoutBytes; ++i) {
                bytes[i] = static_cast<unsigned char>(std::strtoul(line.substr(tab + 1 + i * 2, 2).c_str(), nullptr, 16));
            }
            loaded += AddLayout(line.substr(0, tab), layout, false);
        }
    }
    g_layouts_out = std::fopen(path, header_ok ? "ab" : "wb");
    if (g_layouts_out && !header_ok) {
        std::fprintf(g_layouts_out, "FSLAYOUT %d\n", kLayoutBytes);
    }
    std::fprintf(stderr, "SOTC PREDICT: %zu pixel layouts loaded from %s\n", loaded, path);
}

// The color targets a layout enables (a valid data format), as a mask like Info::mrt_mask.
uint32_t ActiveMask(const FsLayout& layout) {
    uint32_t mask = 0;
    for (size_t i = 0; i < layout.color.size(); ++i) {
        if (layout.color[i].data_format != AmdGpu::DataFormat::FormatInvalid) {
            mask |= 1u << i;
        }
    }
    return mask;
}

FsLayout LayoutOf(const Shader::StageSpecialization& spec) {
    FsLayout layout;
    layout.color = spec.runtime_info.hw.fs.color_buffers;
    layout.mrtz_mask = spec.runtime_info.hw.fs.mrtz_mask;
    layout.num_samples = spec.runtime_info.hw.fs.num_samples;
    return layout;
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

// The input semantic indices of a single-program pixel file, from the name table after its footer: a 16-byte header,
// then 16-byte entries (u32 name offset, u32 semantic offset, desc, 0); desc byte 1 = class (0x24 interpolant),
// byte 2 = semantic index (TEXCOORD n). The VS exports TEXCOORD n at PARAM n, so input slot i (the i-th interpolant
// record) reads PARAM = its semantic index; a PS that skips a VS output thus reads a later param (distortion_ps,
// displacement_ps: slot 5 = TEXCOORD 6). The table of a VS+PS file mixes both programs and is not used.
void ParamsFromTail(const unsigned char* data, size_t n, size_t footer_end, PsPrediction& pred) {
    size_t entry = footer_end + 16;
    uint32_t slot = 0;
    while (entry + 16 <= n && slot < pred.param.size()) {
        const uint32_t desc = Dword(data + entry + 8);
        const uint32_t pad = Dword(data + entry + 12);
        if (pad != 0) {
            break; // the 8-byte last record (outputs) and the names follow
        }
        if (((desc >> 8) & 0xFF) == 0x24) {
            pred.param[slot++] = static_cast<uint8_t>((desc >> 16) & 0xFF);
        }
        entry += 16;
    }
}

void NoteLoadedData(const unsigned char* data, size_t n) noexcept {
    if (!On() || !data || n < 64) {
        return;
    }
    uint32_t programs = 0;
    uint64_t single_hash = 0;
    size_t single_footer = 0;
    static constexpr unsigned char kOrb[7] = {'O', 'r', 'b', 'S', 'h', 'd', 'r'};
    const unsigned char* p = data;
    const unsigned char* end = data + n - 32;
    while (p <= end) {
        const void* hit = std::memchr(p, 'O', static_cast<size_t>(end - p) + 1);
        if (!hit) {
            break;
        }
        p = static_cast<const unsigned char*>(hit);
        if (std::memcmp(p, kOrb, sizeof kOrb) != 0) {
            ++p;
            continue;
        }
        const uint32_t bits = Dword(p + 8);
        uint64_t hash = 0;
        std::memcpy(&hash, p + 16, 8);
        NoteLoaded(hash);
        ++programs;
        single_hash = hash;
        single_footer = static_cast<size_t>(p - data);
        if (((bits >> 2) & 0xF) == 0) { // pixel
            bool known = false;
            {
                std::scoped_lock lock{g_mutex};
                known = g_ps_pred.contains(hash);
            }
            PsPrediction pred;
            if (!known && PredictPixel(data, static_cast<size_t>(p - data), hash, bits >> 8, pred)) {
                std::scoped_lock lock{g_mutex};
                g_ps_pred.try_emplace(hash, pred);
            }
        }
        p += 32;
    }
    if (programs == 1) {
        std::scoped_lock lock{g_mutex};
        if (const auto it = g_ps_pred.find(single_hash); it != g_ps_pred.end()) {
            ParamsFromTail(data, n, single_footer + 32, it->second);
        }
    }
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
        const FsLayout layout = LayoutOf(spec);
        const auto& set = g_pass_layouts[entry->second.pass];
        c.layout_set_sum += set.size();
        bool hit = false;
        for (const FsLayout& known : set) {
            hit = hit || known == layout;
        }
        ++(hit ? c.layout_hit : c.layout_miss);
        // The pass-free rule: every known layout (any pass) whose enabled targets equal the targets the shader
        // exports (Info::mrt_mask), plus the layout without targets (depth-only draws of color-exporting shaders).
        bool rule = ActiveMask(layout) == 0;
        uint64_t cands = 1; // the no-target layout
        std::vector<const FsLayout*> seen;
        for (const auto& [pass, known_set] : g_pass_layouts) {
            for (const FsLayout& known : known_set) {
                const uint32_t mask = ActiveMask(known);
                if (mask == 0 || mask != info.mrt_mask) {
                    continue;
                }
                bool dup = false;
                for (const FsLayout* other : seen) {
                    dup = dup || *other == known;
                }
                if (dup) {
                    continue;
                }
                seen.push_back(&known);
                ++cands;
                rule = rule || known == layout;
            }
        }
        ++(rule ? c.rule_hit : c.rule_miss);
        c.rule_cands_sum += cands;
        c.union_hit += rule || hit;
        if (!hit) {
            AddLayout(entry->second.pass, layout, true);
        }
    }
    if (info.sw_stage == Shader::SwStage::Fragment) {
        if (const auto pred = g_ps_pred.find(info.pgm_hash); pred != g_ps_pred.end()) {
            const PsPrediction& pr = pred->second;
            const auto& rt = spec.runtime_info;
            const auto& fs = rt.hw.fs;
            ++c.pred_found;
            const bool user = ((pr.rsrc2 >> 1) & 0x1F) == rt.props.num_user_data;
            const bool vgprs = ((pr.rsrc1 & 0x3F) + 1) * 4 == rt.props.num_allocated_vgprs;
            const bool fp = ((pr.rsrc1 >> 12) & 3) == static_cast<uint32_t>(rt.props.fp_round_mode32) &&
                            ((pr.rsrc1 >> 14) & 3) == static_cast<uint32_t>(rt.props.fp_round_mode16_64) &&
                            ((pr.rsrc1 >> 16) & 3) == static_cast<uint32_t>(rt.props.fp_denorm_mode32) &&
                            ((pr.rsrc1 >> 18) & 3) == static_cast<uint32_t>(rt.props.fp_denorm_mode16_64);
            uint32_t ena = 0, addr = 0;
            std::memcpy(&ena, &fs.en_flags, sizeof ena);
            std::memcpy(&addr, &fs.addr_flags, sizeof addr);
            const bool ena_ok = ena == pr.ena;
            const bool addr_ok = addr == pr.addr;
            const uint32_t clip = fs.clip_distance_emulation ? 1u : 0u;
            const uint32_t count = fs.num_inputs > clip ? fs.num_inputs - clip : 0;
            uint32_t predicted_count = 0;
            for (uint32_t i = 0; i < 64; ++i) {
                if (pr.read_mask & (uint64_t{1} << i)) {
                    predicted_count = i + 1;
                }
            }
            const bool ninputs = predicted_count == count;
            const bool ninterp = pr.ninterp == count;
            bool inputs = ninterp;
            for (uint32_t i = 0; inputs && i < count && i < fs.inputs.size(); ++i) {
                const auto& in = fs.inputs[i];
                const bool interpolated = (pr.interp_mask >> i) & 1;
                inputs = in.param_index == pr.param[i] && in.is_flat == !interpolated &&
                         in.is_default == !interpolated &&
                         (interpolated || in.default_value == 0);
            }
            if (!inputs && ninterp) {
                static int shown = 0;
                if (shown++ < 8) {
                    std::string slots;
                    for (uint32_t i = 0; i < count && i < fs.inputs.size(); ++i) {
                        const auto& in = fs.inputs[i];
                        char b[48];
                        std::snprintf(b, sizeof b, " %u:p%u/d%u/f%u/v%u", i, unsigned{in.param_index},
                                      unsigned{in.is_default}, unsigned{in.is_flat}, unsigned{in.default_value});
                        slots += b;
                    }
                    std::fprintf(stderr, "SOTC PREDICT input miss %#llx: read %#llx interp %#llx;%s\n",
                                 static_cast<unsigned long long>(info.pgm_hash),
                                 static_cast<unsigned long long>(pr.read_mask),
                                 static_cast<unsigned long long>(pr.interp_mask), slots.c_str());
                }
            }
            c.pred_user += user;
            c.pred_vgprs += vgprs;
            c.pred_fp += fp;
            c.pred_ena += ena_ok;
            c.pred_addr += addr_ok;
            c.pred_ninputs += ninputs;
            c.pred_ninterp += ninterp;
            c.pred_inputs += inputs;
            c.pred_all += user && vgprs && fp && ena_ok && addr_ok && inputs;
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

void OnCachedProgram(const Shader::Info& info, const Shader::StageSpecialization& spec) noexcept {
    if (!On() || info.sw_stage != Shader::SwStage::Fragment) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_table_loaded) {
        LoadTable();
    }
    if (const auto entry = g_table.find(info.pgm_hash); entry != g_table.end()) {
        AddLayout(entry->second.pass, LayoutOf(spec), true);
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
        char line[900];
        std::snprintf(line, sizeof line,
                      "SOTC PREDICT %s (session, %.0f s window): translations %llu (new permutations %llu); known at "
                      "load %llu (lead avg %llu ms, max %llu ms); in table %llu; pass layout in set %llu / miss %llu "
                      "(avg set %.2f; %zu layouts over %zu passes); export-mask rule %llu / miss %llu (avg candidates %.2f), "
                      "rule or set %llu; images %llu (not 2D %llu, integer %llu, sRGB %llu, storage %llu); buffers "
                      "%llu (formatted %llu, not 32-bit float %llu); loaded programs %zu\n",
                      kNames[s], seconds, (unsigned long long)c.translations, (unsigned long long)c.permutations,
                      (unsigned long long)c.known, (unsigned long long)(c.known ? c.lead_ms_sum / c.known : 0),
                      (unsigned long long)c.lead_ms_max, (unsigned long long)c.in_table,
                      (unsigned long long)c.layout_hit, (unsigned long long)c.layout_miss,
                      (c.layout_hit + c.layout_miss) ? double(c.layout_set_sum) / double(c.layout_hit + c.layout_miss)
                                                     : 0.0,
                      g_layout_count, g_pass_layouts.size(), (unsigned long long)c.rule_hit,
                      (unsigned long long)c.rule_miss,
                      (c.rule_hit + c.rule_miss) ? double(c.rule_cands_sum) / double(c.rule_hit + c.rule_miss) : 0.0,
                      (unsigned long long)c.union_hit, (unsigned long long)c.images,
                      (unsigned long long)c.img_not_2d, (unsigned long long)c.img_integer,
                      (unsigned long long)c.img_srgb, (unsigned long long)c.img_storage,
                      (unsigned long long)c.buffers, (unsigned long long)c.buf_formatted,
                      (unsigned long long)c.buf_other_format, g_loaded.size());
        out += line;
        if (c.pred_found) {
            std::snprintf(line, sizeof line,
                          "SOTC PREDICT %s registers (step 2a): predicted %llu of %llu; user SGPRs %llu, VGPRs %llu, "
                          "fp modes %llu, PS_INPUT_ENA %llu, PS_INPUT_ADDR %llu, input count from code %llu, from "
                          "file %llu, inputs %llu; all registers and inputs %llu\n",
                          kNames[s], (unsigned long long)c.pred_found, (unsigned long long)c.translations,
                          (unsigned long long)c.pred_user, (unsigned long long)c.pred_vgprs,
                          (unsigned long long)c.pred_fp, (unsigned long long)c.pred_ena,
                          (unsigned long long)c.pred_addr, (unsigned long long)c.pred_ninputs,
                          (unsigned long long)c.pred_ninterp, (unsigned long long)c.pred_inputs,
                          (unsigned long long)c.pred_all);
            out += line;
        }
    }
}

} // namespace SotcPredict
