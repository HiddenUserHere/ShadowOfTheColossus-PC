// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RES_DUMP=<dir> (off by default; observe only, nothing is changed). Writes the bytes of selected game
// resources as the loader hands them over, so the game's own files can be studied offline. The files are derived
// from game data: <dir> must be outside the repository (the default user/re_dump is under user/, gitignored).
//
// Two sources:
//  1. Resources (RES_LOADED, probe_csdr.cpp). The loader 0x147160 (ResourceFactory::CreateResource) calls the
//     memory-stream constructor 0x4d470 with the data and size of every resource it reads, stored or decompressed
//     (rdx, rcx). The constructor does not get the name, so RES_CREATE (entry of 0x147160, scripts/hooks.d/dump.py)
//     keeps the loader's request (rsi = param_2) in a thread-local slot. The name is the C string at request+0xa8 (the
//     loader passes it to a "%s" in its error message); request+0x98 is tried as inline text. The type is the name's
//     extension (".cmat" gives cmat). RES_LOADED is taken only when its return address is the loader's call site
//     (0x1472ae + 5 = 0x1472b3), so the other callers of 0x4d470 are skipped. Needs SOTC_RES_LOADED=1 (the switch of
//     the RES_LOADED hook; sotc.py sets it for its probe switches).
//  2. The workspace dependency table (the .cwks body). LoadResourceDependencies 0x14efb0 reads the file with 0x1512b0
//     and keeps the body at pool+0x130 (dep_end at pool+0x128). It is not a resource, so it never reaches RES_LOADED.
//     CWKS_BODY (entry of RecordResourceDependencies 0x14edc0, rdi = the pool) dumps it once, before the job frees it.
//     The table has no size field, so the extent is the largest end of any name string or dependency list: entries at
//     table+4 (16 bytes: key, name_off, dep_count, deps_off), offsets from dep_end. Bytes after the last string are not
//     in the dump.
//
// SOTC_RES_DUMP_TYPES: comma list of extensions, default "cprt,cwks,cmat" (the body counts as cwks).
// SOTC_RES_DUMP_MAX: files per type, default 20.
// Output: one line per file on stderr, "SOTC RES DUMP <type> <bytes> <file> <name>".
// File names: <type>_<seq>_<fnv of the name>.bin, cwks_body_<seq>_<fnv of the bytes>.bin.

#include "probe_dump.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "guest_mem.h"
#include "hooks.h"

namespace SotcResDump {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kLoaderCallReturn = 0x1472b3; // return address after the call of 0x4d470 in 0x147160
constexpr uint64_t kNameOffset = 0xa8;           // request+0xa8: char* name (the loader's "%s")
constexpr uint64_t kInlineNameOffset = 0x98;     // request+0x98: name object, inline text
constexpr uint64_t kMaxBody = uint64_t{64} << 20;
constexpr size_t kMaxName = 400;

struct Config {
    bool on = false;
    std::string dir;
    std::vector<std::string> types;
    int max_per_type = 20;
};

// Reads the switches once. Creates the directory; a directory that cannot be created turns the dump off.
const Config& Cfg() {
    static const Config cfg = [] {
        Config c;
        const char* dir = std::getenv("SOTC_RES_DUMP");
        if (!dir || !*dir || std::strcmp(dir, "0") == 0) {
            return c;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "SOTC RES DUMP: cannot create %s, dump off\n", dir);
            return c;
        }
        c.dir = dir;
        const char* types = std::getenv("SOTC_RES_DUMP_TYPES");
        const std::string list = types && *types ? types : "cprt,cwks,cmat";
        std::string token;
        for (size_t i = 0; i <= list.size(); ++i) {
            const char ch = i < list.size() ? list[i] : ',';
            if (ch == ',' || ch == ' ') {
                if (!token.empty()) {
                    c.types.push_back(token);
                    token.clear();
                }
            } else {
                token += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            }
        }
        if (const char* m = std::getenv("SOTC_RES_DUMP_MAX")) {
            const int v = std::atoi(m);
            if (v > 0) {
                c.max_per_type = v;
            }
        }
        c.on = true;
        std::fprintf(stderr, "SOTC RES DUMP: writing to %s, types %s, max %d per type\n", c.dir.c_str(), list.c_str(),
                     c.max_per_type);
        return c;
    }();
    return cfg;
}

std::mutex g_mutex; // guards g_counts and the log lines
std::map<std::string, int> g_counts;
std::atomic<uint64_t> g_seq{0};
thread_local uint64_t t_request = 0; // the loader's request of the CreateResource call in progress on this thread

uint64_t Fnv(const unsigned char* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

// Readable bytes at addr, up to the end of its mapped region (at most max). nullptr when unmapped.
const unsigned char* Span(uint64_t addr, uint64_t max, uint64_t* avail) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!addr || !runtime_memory_region(static_cast<uintptr_t>(addr), &start, &end, &mapped) || !mapped ||
        addr < start || addr >= end) {
        return nullptr;
    }
    *avail = end - addr < max ? end - addr : max;
    return runtime_memory_backing_pointer(static_cast<uintptr_t>(addr));
}

// The printable C string at addr (at most kMaxName bytes, NUL terminated), or empty.
std::string ReadCString(uint64_t addr) {
    std::string text;
    while (text.size() < kMaxName) {
        uint64_t avail = 0;
        const unsigned char* p = Span(addr + text.size(), kMaxName - text.size(), &avail);
        if (!p || avail == 0) {
            return {};
        }
        for (uint64_t i = 0; i < avail; ++i) {
            const unsigned char c = p[i];
            if (c == 0) {
                return text;
            }
            if (c < 32 || c >= 127) {
                return {};
            }
            text += static_cast<char>(c);
        }
    }
    return {};
}

// The resource name of a loader request: the char* at +0xa8, else the inline text at +0x98.
std::string RequestName(uint64_t request) {
    uint64_t p = 0;
    if (SotcGuest::Read(request + kNameOffset, &p) && p) {
        std::string name = ReadCString(p);
        if (!name.empty()) {
            return name;
        }
    }
    std::string inline_text;
    for (uint64_t i = 0; i < 120; ++i) {
        unsigned char c = 0;
        if (!SotcGuest::Read(request + kInlineNameOffset + i, &c, 1) || c == 0) {
            break;
        }
        if (c < 32 || c >= 127) {
            return {};
        }
        inline_text += static_cast<char>(c);
    }
    return inline_text.size() >= 4 ? inline_text : std::string{};
}

// Reserves one file of the type: false when the type is not selected or its cap is reached.
bool Reserve(const std::string& type) {
    const Config& cfg = Cfg();
    bool selected = false;
    for (const std::string& t : cfg.types) {
        selected = selected || t == type;
    }
    if (!selected) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    int& n = g_counts[type];
    if (n >= cfg.max_per_type) {
        return false;
    }
    ++n;
    return true;
}

// Writes data to <dir>/<file> and logs one line.
void WriteFile(const std::string& file, const std::string& type, const unsigned char* data, size_t n,
               const std::string& name) {
    const std::string path = Cfg().dir + "/" + file;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::scoped_lock lock{g_mutex};
        std::fprintf(stderr, "SOTC RES DUMP: cannot write %s\n", path.c_str());
        return;
    }
    std::fwrite(data, 1, n, f);
    std::fclose(f);
    std::scoped_lock lock{g_mutex};
    std::fprintf(stderr, "SOTC RES DUMP %s %zu %s %s\n", type.c_str(), n, path.c_str(), name.c_str());
}

// RES_CREATE: entry of the loader 0x147160; rsi is the request (param_2).
HookAction OnCreate(GuestRegs& regs, void*) {
    t_request = regs.rsi;
    return HookAction::Continue;
}

// CWKS_BODY: entry of 0x14edc0 (RecordResourceDependencies); rdi is the pool.
HookAction OnRecord(GuestRegs& regs, void*) {
    static std::atomic<bool> done{false};
    if (done.exchange(true) || !Cfg().on || !Reserve("cwks")) {
        return HookAction::Continue;
    }
    const uint64_t pool = regs.rdi;
    uint64_t table = 0, dep_end = 0;
    uint32_t count = 0;
    if (!SotcGuest::Read(pool + 0x130, &table) || !SotcGuest::Read(pool + 0x128, &dep_end) || !table || !dep_end ||
        !SotcGuest::Read(table, &count) || count == 0 || count > (1u << 20)) {
        return HookAction::Continue;
    }
    // Extent from dep_end: the largest end of a name string or of a dependency list.
    uint64_t extent = 0;
    for (uint32_t i = 0; i < count && extent <= kMaxBody; ++i) {
        const uint64_t entry = table + 4 + uint64_t{i} * 16; // key, name_off, dep_count, deps_off
        uint32_t name_off = 0, dep_count = 0, deps_off = 0;
        if (!SotcGuest::Read(entry + 4, &name_off) || !SotcGuest::Read(entry + 8, &dep_count) ||
            !SotcGuest::Read(entry + 12, &deps_off)) {
            break;
        }
        uint64_t len = 0;
        for (;;) {
            unsigned char c = 0;
            if (len > 4096 || !SotcGuest::Read(dep_end + name_off + len, &c, 1) || c == 0) {
                break;
            }
            ++len;
        }
        if (name_off + len + 1 > extent) {
            extent = name_off + len + 1;
        }
        uint64_t q = deps_off;
        for (uint32_t m = 0; m < dep_count && m < 65536; ++m) {
            unsigned char item = 0;
            if (!SotcGuest::Read(dep_end + q, &item, 1)) {
                break;
            }
            q += uint64_t{item} + 1;
        }
        if (q > extent) {
            extent = q;
        }
    }
    const uint64_t total = (dep_end - table) + extent;
    if (total > kMaxBody) {
        std::scoped_lock lock{g_mutex};
        std::fprintf(stderr, "SOTC RES DUMP: workspace body of %llu bytes is too large, not dumped\n",
                     static_cast<unsigned long long>(total));
        return HookAction::Continue;
    }
    const unsigned char* body = SotcGuest::Backing(table, static_cast<size_t>(total));
    if (!body) {
        std::scoped_lock lock{g_mutex};
        std::fprintf(stderr, "SOTC RES DUMP: workspace body at 0x%llx is not one mapped range, not dumped\n",
                     static_cast<unsigned long long>(table));
        return HookAction::Continue;
    }
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    char file[96];
    std::snprintf(file, sizeof file, "cwks_body_%06llu_%016llx.bin", static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(Fnv(body, static_cast<size_t>(total))));
    char name[64];
    std::snprintf(name, sizeof name, "body entries %u", count);
    WriteFile(file, "cwks", body, static_cast<size_t>(total), name);
    return HookAction::Continue;
}

} // namespace

bool On() noexcept {
    return Cfg().on;
}

void NoteResource(uint64_t ret, const unsigned char* data, size_t n) noexcept {
    const uint64_t request = t_request;
    const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
    if (!request || !base || ret != base + kLoaderCallReturn || !data || n == 0) {
        return;
    }
    t_request = 0;
    const std::string name = RequestName(request);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return;
    }
    std::string type;
    for (size_t i = dot + 1; i < name.size(); ++i) {
        const char ch = name[i];
        type += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    if (!Reserve(type)) {
        return;
    }
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    char file[96];
    std::snprintf(file, sizeof file, "%s_%06llu_%016llx.bin", type.c_str(), static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(Fnv(reinterpret_cast<const unsigned char*>(name.data()), name.size())));
    WriteFile(file, type, data, n, name);
}

void Register() {
    // Both handlers run only when their hooks are installed (SOTC_RES_DUMP set, scripts/hooks.d/dump.py).
    SotcHooks::RegisterHandler("RES_CREATE", SotcHooks::Handler{OnCreate, nullptr, nullptr});
    SotcHooks::RegisterHandler("CWKS_BODY", SotcHooks::Handler{OnRecord, nullptr, nullptr});
}

} // namespace SotcResDump
