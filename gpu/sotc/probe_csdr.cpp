// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_CSDR_PROBE=1 (off by default; observe only, nothing is changed). Finds the format of the game's
// streamed resources, in particular the .csdr shader files, so the shaders can be found and translated ahead of
// their first draw.
//
// RES_DECOMPRESS_DONE is a site in the resource loader 0x147160, right after its call of the decompress wrapper
// 0x1aa2a0 (OodleLZ_Decompress 0x89d3b0, zlib or a copy by type; the wrapper itself has an indirect jump, so it cannot
// be wrapped). At the site: rax = bytes produced, r14 = raw buffer, r12 = raw size, r15 = the request (+0x80 compressed
// size, +0x90 compressed data, +0x98 name object, +0xf0 the u32 descriptor type << 29 | raw size).
// Per call one line goes to SOTC_CSDR_PROBE_OUT (default user/csdr_probe.tsv):
//   desc type, raw size, compressed size, result, the name (when the name object holds a readable string), FNV-1a 64 of the first 4 KiB of the source (so the file can be
//   found in the .psarc archives offline), the first 16 source bytes, and every OrbShdr footer in the output
//   (shader hash, code length in bytes, footer offset).
// SOTC_CSDR_DUMP_DIR (with SOTC_CSDR_DUMP_MAX, default 64) also writes the decompressed output of the calls that
// hold at least one OrbShdr footer, for studying the format. The dumps are derived from game data: they belong in a
// scratch folder, never in the repository.

#include "probe_csdr.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "guest_mem.h"
#include "hooks.h"
#include "predict_probe.h"
#include "pretranslate.h"
#include "probe_dump.h" // sotc: SOTC_RES_DUMP (RES_LOADED feeds it)

namespace SotcCsdrProbe {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr size_t kKeyBytes = 4096;
constexpr unsigned char kOrb[7] = {'O', 'r', 'b', 'S', 'h', 'd', 'r'};

std::mutex g_out_mutex;
std::FILE* g_out = nullptr;
std::atomic<int> g_dumps{0};
int g_dump_max = 64;
const char* g_dump_dir = nullptr;
std::atomic<uint64_t> g_seq{0};

uint64_t Fnv(const unsigned char* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

void OpenOut() {
    const char* path = std::getenv("SOTC_CSDR_PROBE_OUT");
    g_out = std::fopen(path && *path ? path : "user/csdr_probe.tsv", "wb");
    if (g_out) {
        std::fputs("seq\ttype\traw\tcsize\tresult\tkey_fnv\tname\thead16\tprograms (hash:length:offset)\n", g_out);
    }
    g_dump_dir = std::getenv("SOTC_CSDR_DUMP_DIR");
    if (const char* m = std::getenv("SOTC_CSDR_DUMP_MAX")) {
        g_dump_max = std::atoi(m);
    }
    std::fprintf(stderr, "SOTC CSDR PROBE: writing %s%s%s\n", path && *path ? path : "user/csdr_probe.tsv",
                 g_dump_dir ? ", dumps to " : "", g_dump_dir ? g_dump_dir : "");
}

// A printable run of at least 4 characters at addr (up to 120), or empty.
std::string ReadName(uint64_t addr) {
    char text[128] = {};
    if (!SotcGuest::Read(addr, text, 120)) {
        return {};
    }
    size_t n = 0;
    while (n < 120 && text[n] >= 32 && text[n] < 127) {
        ++n;
    }
    return n >= 4 ? std::string(text, n) : std::string{};
}

// The name object at request+0x98: tried as an inline string, as a char pointer and as a pointer to one.
std::string RequestName(uint64_t request) {
    std::string name = ReadName(request + 0x98);
    uint64_t p = 0;
    if (name.empty() && SotcGuest::Read(request + 0x98, &p, sizeof p) && p) {
        name = ReadName(p);
        uint64_t q = 0;
        if (name.empty() && SotcGuest::Read(p, &q, sizeof q) && q) {
            name = ReadName(q);
        }
    }
    for (char& c : name) {
        if (c == '\t') {
            c = ' ';
        }
    }
    return name;
}

HookAction Site(GuestRegs& regs, void*) {
    // SOTC_CSDR_EXTRACT: the first decompression of the session runs the whole extraction here, on the game's
    // loader thread, once the eboot's own initialization (Oodle's CPU dispatch among it) has run; then the process
    // ends. Before the game starts, OodleLZ_Decompress fails on the Mermaid files (type 3).
    static std::atomic<bool> extract_tried{false};
    if (std::getenv("SOTC_CSDR_EXTRACT") && !extract_tried.exchange(true)) {
        const int code = ExtractIfRequested(SotcHooks::ImageBase());
        std::fflush(stderr);
        std::_Exit(code < 0 ? 0 : code);
    }
    struct {
        uint32_t desc{};
        uint64_t src{}, csize{}, dst{};
    } f;
    const uint64_t request = regs.r15;
    SotcGuest::Read(request + 0xf0, &f.desc, sizeof f.desc);
    SotcGuest::Read(request + 0x80, &f.csize, sizeof f.csize);
    SotcGuest::Read(request + 0x90, &f.src, sizeof f.src);
    f.dst = regs.r14;
    const uint32_t type = f.desc >> 29;
    const uint32_t raw = f.desc & 0x1fffffffu;
    const uint64_t result = regs.rax;

    std::string line;
    char buf[160];
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    const size_t key_n = static_cast<size_t>(f.csize < kKeyBytes ? f.csize : kKeyBytes);
    const unsigned char* src = SotcGuest::Backing(f.src, key_n);
    const uint64_t key = src ? Fnv(src, key_n) : 0;
    std::snprintf(buf, sizeof buf, "%llu\t%u\t%u\t%llu\t%llu\t%016llx\t", static_cast<unsigned long long>(seq), type,
                  raw, static_cast<unsigned long long>(f.csize), static_cast<unsigned long long>(result),
                  static_cast<unsigned long long>(key));
    line += buf;
    line += RequestName(request);
    line += '\t';
    for (size_t i = 0; src && i < 16 && i < key_n; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", src[i]);
        line += buf;
    }
    line += '\t';

    // The OrbShdr footers of the output (BinaryInfo, regs_shader.h: signature 7, version 1, bitfield u32 whose
    // top 24 bits are the code length, 4 bytes, shader_hash u64 at +16, crc32 at +24).
    int programs = 0;
    const size_t out_n = static_cast<size_t>(result != 0 && result <= raw ? result : raw);
    const unsigned char* out = SotcGuest::Backing(f.dst, out_n);
    if (out && out_n >= 32) {
        const unsigned char* p = out;
        const unsigned char* end = out + out_n - 32;
        while (p <= end) {
            const void* hit = std::memchr(p, 'O', static_cast<size_t>(end - p) + 1);
            if (!hit) {
                break;
            }
            p = static_cast<const unsigned char*>(hit);
            if (std::memcmp(p, kOrb, sizeof kOrb) == 0) {
                uint32_t bits = 0;
                uint64_t hash = 0;
                std::memcpy(&bits, p + 8, 4);
                std::memcpy(&hash, p + 16, 8);
                SotcPredict::NoteLoaded(hash); // sotc: SOTC_PREDICT_PROBE (predict_probe.cpp)
                std::snprintf(buf, sizeof buf, "%s%016llx:%u:%llu", programs ? "," : "",
                              static_cast<unsigned long long>(hash), bits >> 8,
                              static_cast<unsigned long long>(p - out));
                line += buf;
                ++programs;
                p += 32;
            } else {
                ++p;
            }
        }
    }
    line += '\n';

    std::scoped_lock lock{g_out_mutex};
    if (g_out) {
        std::fputs(line.c_str(), g_out);
        std::fflush(g_out);
    }
    if (programs && g_dump_dir && out && g_dumps.load(std::memory_order_relaxed) < g_dump_max) {
        g_dumps.fetch_add(1, std::memory_order_relaxed);
        std::snprintf(buf, sizeof buf, "%s/csdr_%06llu_%016llx.bin", g_dump_dir, static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(key));
        if (std::FILE* d = std::fopen(buf, "wb")) {
            std::fwrite(out, 1, out_n, d);
            std::fclose(d);
        }
    }
    return HookAction::Continue;
}

// RES_LOADED: entry of the memory-stream constructor 0x4d470 that gets every resource the loader 0x147160 reads,
// stored or decompressed (rdx = data, rcx = size). The predict probe (the hashes of the data's OrbShdr footers) and
// SOTC_PRETRANSLATE (the programs, translated at load) use it. The data is the loader's stream (CPU heap), freed
// once the resource is built: the GPU bytes are copied out of it (SOTC_PRELOAD_UPLOAD hooks that copy instead).
HookAction Loaded(GuestRegs& regs, void*) {
    const bool dump = SotcResDump::On(); // sotc: SOTC_RES_DUMP (probe_dump.cpp)
    if (!SotcPredict::On() && !SotcPretranslate::On() && !dump) {
        return HookAction::Continue;
    }
    const size_t n = static_cast<size_t>(regs.rcx);
    const unsigned char* data = n >= 32 && n < (size_t{1} << 30) ? SotcGuest::Backing(regs.rdx, n) : nullptr;
    if (!data) {
        return HookAction::Continue;
    }
    if (dump) {
        uint64_t ret = 0; // sotc: SOTC_RES_DUMP, the return address at [rsp] names the loader's call site
        std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof ret);
        SotcResDump::NoteResource(ret, data, n);
    }
    SotcPredict::NoteLoadedData(data, n);
    return HookAction::Continue;
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_CSDR_PROBE");
        return v && v[0] == '1';
    }();
    return on;
}

int ExtractIfRequested(uintptr_t image_base) {
    const char* in_path = std::getenv("SOTC_CSDR_EXTRACT");
    if (!in_path || !*in_path) {
        return -1;
    }
    const char* out_path = std::getenv("SOTC_CSDR_EXTRACT_OUT");
    if (!out_path || !*out_path) {
        std::fprintf(stderr, "SOTC CSDR EXTRACT: SOTC_CSDR_EXTRACT_OUT is not set\n");
        return 2;
    }
    // EU 1.01 / US 1.00 eboot: OodleLZ_Decompress at 0x89d3b0, OodleLZDecoder_MemorySizeNeeded at 0x89b980. Their
    // first bytes are checked, so another eboot refuses instead of calling into the wrong code.
    constexpr uint64_t kDecompress = 0x89d3b0, kMemSize = 0x89b980;
    static const unsigned char kDecompressHead[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec};
    static const unsigned char kMemSizeHead[] = {0x55, 0x48, 0x89, 0xe5, 0x31, 0xd2, 0xe8, 0x05};
    if (std::memcmp(reinterpret_cast<const void*>(image_base + kDecompress), kDecompressHead,
                    sizeof kDecompressHead) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(image_base + kMemSize), kMemSizeHead, sizeof kMemSizeHead) != 0) {
        std::fprintf(stderr, "SOTC CSDR EXTRACT: the Oodle entry points do not match this eboot\n");
        return 3;
    }
    using DecompressFn = int64_t(__attribute__((sysv_abi)) *)(const void* comp, int64_t comp_len, void* raw,
                                                               int64_t raw_len, int fuzz_safe, int check_crc,
                                                               int verbosity, void* dec_base, int64_t dec_size,
                                                               void* callback, void* callback_user, void* decoder_mem,
                                                               int64_t decoder_mem_size, int thread_phase);
    using MemSizeFn = int(__attribute__((sysv_abi)) *)(int compressor, int64_t raw_len);
    const auto decompress = reinterpret_cast<DecompressFn>(image_base + kDecompress);
    const auto mem_size = reinterpret_cast<MemSizeFn>(image_base + kMemSize);

    std::FILE* in = std::fopen(in_path, "rb");
    std::FILE* out = std::fopen(out_path, "wb");
    if (!in || !out) {
        std::fprintf(stderr, "SOTC CSDR EXTRACT: cannot open %s or %s\n", in_path, out_path);
        return 4;
    }
    char magic[8] = {};
    uint32_t count = 0;
    if (std::fread(magic, 1, 8, in) != 8 || std::memcmp(magic, "CSDRIN01", 8) != 0 ||
        std::fread(&count, 4, 1, in) != 1) {
        std::fprintf(stderr, "SOTC CSDR EXTRACT: %s is not a CSDRIN01 pack\n", in_path);
        return 5;
    }
    const int decoder_size = mem_size(-1, -1);
    std::string decoder(static_cast<size_t>(decoder_size > 0 ? decoder_size : 0), '\0');
    std::fwrite("CSDROUT1", 1, 8, out);
    std::fwrite(&count, 4, 1, out);
    uint32_t ok = 0, failed = 0, stored = 0;
    uint64_t raw_total = 0;
    std::string name, comp, raw;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t name_len = 0, desc = 0, csize = 0;
        if (std::fread(&name_len, 4, 1, in) != 1) {
            break;
        }
        name.resize(name_len);
        std::fread(name.data(), 1, name_len, in);
        std::fread(&desc, 4, 1, in);
        std::fread(&csize, 4, 1, in);
        comp.resize(csize);
        std::fread(comp.data(), 1, csize, in);
        const uint32_t type = desc >> 29, raw_len = desc & 0x1fffffffu;
        uint32_t produced = 0;
        if (type == 0) {
            raw = comp; // stored
            produced = csize;
            ++stored;
        } else if (type >= 2 && type <= 5) {
            raw.assign(raw_len, '\0');
            const int64_t r = decompress(comp.data(), csize, raw.data(), raw_len, type == 5 ? 0 : 1, 0, 0, nullptr, 0,
                                         nullptr, nullptr, decoder.empty() ? nullptr : decoder.data(),
                                         static_cast<int64_t>(decoder.size()), 3);
            produced = r == raw_len ? raw_len : 0;
            if (!produced && failed < 6) {
                std::fprintf(stderr,
                             "SOTC CSDR EXTRACT: %s type %u csize %u raw %u -> %lld (first bytes %02x %02x %02x %02x)\n",
                             name.c_str(), type, csize, raw_len, static_cast<long long>(r),
                             csize > 0 ? static_cast<unsigned char>(comp[0]) : 0,
                             csize > 1 ? static_cast<unsigned char>(comp[1]) : 0,
                             csize > 2 ? static_cast<unsigned char>(comp[2]) : 0,
                             csize > 3 ? static_cast<unsigned char>(comp[3]) : 0);
            }
        }
        if (produced) {
            ++ok;
            raw_total += produced;
        } else {
            ++failed;
            raw.clear();
        }
        std::fwrite(&name_len, 4, 1, out);
        std::fwrite(name.data(), 1, name_len, out);
        std::fwrite(&produced, 4, 1, out);
        std::fwrite(raw.data(), 1, produced, out);
        if ((i + 1) % 2000 == 0) {
            std::fprintf(stderr, "SOTC CSDR EXTRACT: %u/%u\n", i + 1, count);
        }
    }
    std::fclose(in);
    std::fclose(out);
    std::fprintf(stderr, "SOTC CSDR EXTRACT: %u records, %u ok (%u stored), %u failed, %.1f MB raw -> %s\n", count,
                 ok, stored, failed, raw_total / 1e6, out_path);
    return failed ? 1 : 0;
}

void Register() {
    // RES_LOADED feeds the predict probe and SOTC_PRETRANSLATE (its handler checks them); a hook that is off never
    // dispatches to it.
    SotcHooks::RegisterHandler("RES_LOADED", SotcHooks::Handler{Loaded, nullptr, nullptr});
    if (!On()) {
        return;
    }
    // sotc: with SOTC_PREDICT_PROBE the hook only feeds the predict probe, unless an output file is named.
    if (!SotcPredict::On() || std::getenv("SOTC_CSDR_PROBE_OUT")) {
        OpenOut();
    }
    SotcHooks::RegisterHandler("RES_DECOMPRESS_DONE", SotcHooks::Handler{Site, nullptr, nullptr});
}

} // namespace SotcCsdrProbe
