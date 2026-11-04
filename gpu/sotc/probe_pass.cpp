// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PASS_PROBE=1 (off by default; observe only, nothing is changed). Logs what the game resolves per
// render pass, to confirm the pass map (docs/re-stutters/native/re-pipelines/pass-map.md, sections 3, 6, 7).
// Hooks in scripts/hooks.d/pass.py:
//   PASS_RESOLVE     entry of FUN_000f8990 (eboot 0xf8990). rdi = the material, rsi = the pass object; the pass
//                    slot is k = *(pass+0xf4). One line per distinct (material, k).
//   MATERIAL_LOADED  wrap of the material parser FUN_000f6c90 (0xf6c90). rdi = the material (kept in a per-thread
//                    stack by the pre phase); the post phase sees the return value (1 on success). One line per
//                    material, listing its non-empty slots.
// Every guest read goes through the backing view with a null/unmapped guard (probe_area.cpp pattern).
// Output: "SOTC PASS ..." lines on stderr, at most kLineCap of them, then only a count; a summary line every 5 s.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include "hooks.h"
#include "probes.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

// Pass object: dword k at +0xf4 (pass-map.md section 2).
constexpr uint64_t kPassSlot = 0xf4;
// Material: MatSlot[14] at +0x60, stride 0x48. Slot B = material + 0x60 + 0x48*k (pass-map.md section 3.1).
constexpr uint64_t kSlotBase = 0x60;
constexpr uint64_t kSlotStride = 0x48;
constexpr uint32_t kSlotCount = 14;
constexpr uint64_t kSlotRes = 0x00;  // B+0x00: the resource ref (the .csdr resource object)
constexpr uint64_t kSlotRecs = 0x10; // B+0x10 .. B+0x30: rec[0..4], one per set bit of the 5-bit mask
constexpr int kRecCount = 5;
constexpr uint64_t kSlotKind = 0x38; // B+0x38: the kind word
// Material fields read by the resolver (pass-map.md section 3).
constexpr uint64_t kMatM458 = 0x458;
constexpr uint64_t kMatM45c = 0x45c;
constexpr uint64_t kMatM460 = 0x460;
constexpr uint64_t kMatM468 = 0x468;
// rec+0x10: pointer to the copied program bytes; the program ends in an OrbShdr footer (predict_table.py).
constexpr uint64_t kProgramBytes = 0x10;
constexpr size_t kProgramScan = size_t{1} << 20; // footer search bound, bytes
constexpr char kOrb[] = "OrbShdr";                // 7 bytes, the footer signature
constexpr size_t kOrbLen = 7;
// Resource name search: a window of the resource object, then the C strings its pointers reach.
constexpr size_t kNameWindow = 0x200;
constexpr int kNamePointers = 64;
constexpr size_t kNameCap = 96;
constexpr size_t kStringCap = 256;

// Output and dedup bounds.
constexpr int kLineCap = 2000;
constexpr size_t kKeyCap = size_t{1} << 18;
constexpr size_t kMaterialCap = size_t{1} << 16;
constexpr int kMatDepth = 16; // open MATERIAL_LOADED calls kept per thread
constexpr double kSummarySeconds = 5.0;

// Reads n bytes of guest memory through the backing view, bounded to the mapped region. False when unmapped or
// partly outside the region.
bool ReadGuest(uintptr_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!address || !runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || address + n > end) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

bool ReadU32(uintptr_t address, uint32_t* out) {
    return ReadGuest(address, out, sizeof *out);
}

bool ReadU64(uintptr_t address, uint64_t* out) {
    return ReadGuest(address, out, sizeof *out);
}

// Copies a NUL-terminated guest C string into out (always terminated), bounded to the mapped region.
bool ReadGuestString(uintptr_t address, char* out, size_t cap) {
    out[0] = '\0';
    if (!address || cap == 0) {
        return false;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped || address < start) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    const size_t avail = static_cast<size_t>(end - address);
    const size_t limit = std::min(cap - 1, avail);
    size_t i = 0;
    for (; i < limit && p[i] != 0; ++i) {
        out[i] = static_cast<char>(p[i]);
    }
    out[i] = '\0';
    return true;
}

// Finds a ".csdr" name in s and returns its basename (the path part before the last '/' is dropped). The name is
// the run of printable characters ending at ".csdr".
bool NameIn(std::string_view s, char* out, size_t cap) {
    const size_t dot = s.find(".csdr");
    if (dot == std::string_view::npos) {
        return false;
    }
    size_t begin = dot;
    while (begin > 0 && s[begin - 1] >= 0x21 && s[begin - 1] <= 0x7e && dot + 5 - begin < 512) {
        --begin;
    }
    std::string_view name = s.substr(begin, dot + 5 - begin);
    const size_t slash = name.rfind('/');
    if (slash != std::string_view::npos) {
        name = name.substr(slash + 1);
    }
    if (name.size() >= cap) {
        name = name.substr(name.size() - (cap - 1)); // keep the end, which holds the pass stem and hash
    }
    std::memcpy(out, name.data(), name.size());
    out[name.size()] = '\0';
    return true;
}

// The .csdr name of a resource object: first a window of its own bytes, then the C strings its 8-byte values
// point at (at most kNamePointers reads). False when no ".csdr" name is reachable.
bool ResourceName(uintptr_t res, char* out, size_t cap) {
    out[0] = '\0';
    if (!res) {
        return false;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(res, &start, &end, &mapped) || !mapped || res < start) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(res);
    if (!p) {
        return false;
    }
    const size_t n = std::min(kNameWindow, static_cast<size_t>(end - res));
    if (NameIn(std::string_view(reinterpret_cast<const char*>(p), n), out, cap)) {
        return true;
    }
    int tried = 0;
    for (size_t o = 0; o + 8 <= n && tried < kNamePointers; o += 8) {
        uint64_t v = 0;
        std::memcpy(&v, p + o, sizeof v);
        if (v < 0x10000) {
            continue;
        }
        char buf[kStringCap];
        if (!ReadGuestString(static_cast<uintptr_t>(v), buf, sizeof buf)) {
            continue;
        }
        ++tried;
        if (NameIn(buf, out, cap)) {
            return true;
        }
    }
    return false;
}

// The OrbShdr footer of the program at prog (rec+0x10): type (bits 2-5 of the bitfield at +8), code length
// (bits 8-31) and the 64-bit hash at +16. The first signature inside the bound is the footer.
struct Footer {
    uint32_t type = 0;
    uint32_t length = 0;
    uint64_t hash = 0;
};

bool FindFooter(uintptr_t prog, Footer* out) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!prog || !runtime_memory_region(prog, &start, &end, &mapped) || !mapped || prog < start) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(prog);
    if (!p) {
        return false;
    }
    const size_t n = std::min(kProgramScan, static_cast<size_t>(end - prog));
    if (n < 32) {
        return false;
    }
    size_t i = 0;
    while (i + 32 <= n) {
        const void* hit = std::memchr(p + i, 'O', n - i - 31);
        if (!hit) {
            break;
        }
        i = static_cast<size_t>(static_cast<const unsigned char*>(hit) - p);
        if (std::memcmp(p + i, kOrb, kOrbLen) == 0) {
            uint32_t bits = 0;
            uint64_t hash = 0;
            std::memcpy(&bits, p + i + 8, sizeof bits);
            std::memcpy(&hash, p + i + 16, sizeof hash);
            out->type = (bits >> 2) & 0xf;
            out->length = bits >> 8;
            out->hash = hash;
            return true;
        }
        ++i;
    }
    return false;
}

// One material slot as the resolver sees it.
struct Slot {
    uintptr_t base = 0;
    uint64_t res = 0;
    uint64_t recs[kRecCount] = {};
    uint32_t kind = 0;
    int present = 0; // number of non-null record pointers
};

bool ReadSlot(uintptr_t mat, uint32_t k, Slot* s) {
    s->base = mat + kSlotBase + kSlotStride * k;
    if (!ReadU64(s->base + kSlotRes, &s->res)) {
        return false;
    }
    for (int i = 0; i < kRecCount; ++i) {
        s->recs[i] = 0;
        if (ReadU64(s->base + kSlotRecs + 8 * static_cast<uint64_t>(i), &s->recs[i]) && s->recs[i]) {
            ++s->present;
        }
    }
    s->kind = 0;
    ReadU32(s->base + kSlotKind, &s->kind);
    return true;
}

// " rec[i]=<type>:<hash>" for each non-null record pointer of a slot, "?" when its footer is not found.
void AppendRecords(std::string& line, const Slot& s) {
    char buf[160];
    for (int i = 0; i < kRecCount; ++i) {
        if (!s.recs[i]) {
            continue;
        }
        uint64_t prog = 0;
        Footer f;
        if (ReadU64(static_cast<uintptr_t>(s.recs[i]) + kProgramBytes, &prog) && FindFooter(static_cast<uintptr_t>(prog), &f)) {
            std::snprintf(buf, sizeof buf, " rec[%d]=%u:%016llx(len %u)", i, static_cast<unsigned>(f.type),
                          static_cast<unsigned long long>(f.hash), static_cast<unsigned>(f.length));
        } else {
            std::snprintf(buf, sizeof buf, " rec[%d]=?:rec=0x%llx", i, static_cast<unsigned long long>(s.recs[i]));
        }
        line += buf;
    }
}

// Shared state, guarded by g_mutex.
std::mutex g_mutex;
std::unordered_set<uint64_t> g_seen_keys;      // (material << 4) | k, logged by PASS_RESOLVE
std::unordered_set<uint64_t> g_seen_materials; // materials logged by MATERIAL_LOADED
int g_lines = 0;                               // "SOTC PASS" lines printed
uint64_t g_dropped = 0;                        // lines past kLineCap
bool g_started = false;
std::chrono::steady_clock::time_point g_window_start;
// Window counters (reset by each 5 s summary).
uint64_t g_resolves = 0;      // PASS_RESOLVE calls
uint64_t g_new_keys = 0;      // of those, distinct (material, k) pairs
uint64_t g_bad_k = 0;         // calls whose k was unreadable or out of 0..13
uint64_t g_materials = 0;     // MATERIAL_LOADED calls with a material
uint64_t g_new_materials = 0; // of those, distinct materials

// Prints one line unless the cap is reached. Caller holds g_mutex.
void Emit(const std::string& line) {
    if (g_lines < kLineCap) {
        std::fprintf(stderr, "%s\n", line.c_str());
        ++g_lines;
    } else {
        ++g_dropped;
    }
}

// Prints the 5 s summary when the window is over. Caller holds g_mutex.
void MaybeSummary(std::chrono::steady_clock::time_point now) {
    if (!g_started) {
        g_started = true;
        g_window_start = now;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - g_window_start).count();
    if (elapsed < kSummarySeconds) {
        return;
    }
    if (g_resolves || g_materials || g_bad_k) {
        std::fprintf(stderr,
                     "SOTC PASS summary: %llu resolves (%llu new mat/k, %llu bad k), %llu materials (%llu new); "
                     "lines %d, past cap %llu, in %.0f s\n",
                     static_cast<unsigned long long>(g_resolves), static_cast<unsigned long long>(g_new_keys),
                     static_cast<unsigned long long>(g_bad_k), static_cast<unsigned long long>(g_materials),
                     static_cast<unsigned long long>(g_new_materials), g_lines,
                     static_cast<unsigned long long>(g_dropped), elapsed);
    }
    g_resolves = g_new_keys = g_bad_k = g_materials = g_new_materials = 0;
    g_window_start = now;
}

// PASS_RESOLVE: entry of FUN_000f8990(material, pass).
HookAction OnResolve(GuestRegs& regs, void*) {
    const auto mat = static_cast<uintptr_t>(regs.rdi);
    const auto pass = static_cast<uintptr_t>(regs.rsi);
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    MaybeSummary(now);
    ++g_resolves;
    uint32_t k = 0;
    if (!ReadU32(pass + kPassSlot, &k) || k >= kSlotCount) {
        ++g_bad_k;
        return HookAction::Continue;
    }
    const uint64_t key = (static_cast<uint64_t>(mat) << 4) | k;
    if (g_seen_keys.size() >= kKeyCap || !g_seen_keys.insert(key).second) {
        return HookAction::Continue;
    }
    ++g_new_keys;

    Slot s;
    ReadSlot(mat, k, &s);
    char name[kNameCap] = {0};
    const bool named = s.res && ResourceName(static_cast<uintptr_t>(s.res), name, sizeof name);

    uint32_t m458 = 0, m45c = 0, m460 = 0, m468 = 0;
    ReadU32(mat + kMatM458, &m458);
    ReadU32(mat + kMatM45c, &m45c);
    ReadU32(mat + kMatM460, &m460);
    ReadU32(mat + kMatM468, &m468);

    char head[400];
    std::snprintf(head, sizeof head,
                  "SOTC PASS k=%u mat=0x%llx pass=0x%llx name=%s res=0x%llx kind=0x%x present=%d "
                  "mkind=%u m458=%u m45c=%u m468=%u",
                  static_cast<unsigned>(k), static_cast<unsigned long long>(mat),
                  static_cast<unsigned long long>(pass), named ? name : "?",
                  static_cast<unsigned long long>(s.res), static_cast<unsigned>(s.kind), s.present,
                  static_cast<unsigned>(m460), static_cast<unsigned>(m458), static_cast<unsigned>(m45c),
                  static_cast<unsigned>(m468));
    std::string line = head;
    if (s.present) {
        AppendRecords(line, s);
    } else {
        line += " recs=none";
    }
    Emit(line);
    return HookAction::Continue;
}

// MATERIAL_LOADED: the material of each open parse, per thread. The pre phase pushes rdi, the post phase pops it.
struct MatStack {
    uintptr_t mat[kMatDepth] = {};
    int depth = 0;
};
thread_local MatStack t_mat;

HookAction OnMaterialPre(GuestRegs& regs, void*) {
    if (t_mat.depth < kMatDepth) {
        t_mat.mat[t_mat.depth] = static_cast<uintptr_t>(regs.rdi);
    }
    ++t_mat.depth;
    return HookAction::Continue;
}

void OnMaterialPost(GuestRegs& regs, void*) {
    --t_mat.depth;
    if (t_mat.depth < 0 || t_mat.depth >= kMatDepth) {
        return; // deeper than the stack: not recorded (the pre phase did not store it)
    }
    const uintptr_t mat = t_mat.mat[t_mat.depth];
    if (!mat) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const uint32_t ret = static_cast<uint32_t>(regs.rax);

    std::lock_guard<std::mutex> lock(g_mutex);
    MaybeSummary(now);
    ++g_materials;
    if (g_seen_materials.size() >= kMaterialCap || !g_seen_materials.insert(mat).second) {
        return;
    }
    ++g_new_materials;

    char head[96];
    std::snprintf(head, sizeof head, "SOTC PASS material mat=0x%llx ret=%u", static_cast<unsigned long long>(mat),
                  static_cast<unsigned>(ret));
    std::string line = head;
    int slots = 0;
    for (uint32_t k = 0; k < kSlotCount; ++k) {
        Slot s;
        if (!ReadSlot(mat, k, &s) || s.present == 0) {
            continue;
        }
        ++slots;
        char name[kNameCap] = {0};
        const bool named = s.res && ResourceName(static_cast<uintptr_t>(s.res), name, sizeof name);
        char buf[200];
        std::snprintf(buf, sizeof buf, " | k=%u name=%s kind=0x%x recs=%d", static_cast<unsigned>(k),
                      named ? name : "?", static_cast<unsigned>(s.kind), s.present);
        line += buf;
    }
    uint32_t m460 = 0, m458 = 0, m45c = 0, m468 = 0;
    ReadU32(mat + kMatM460, &m460);
    ReadU32(mat + kMatM458, &m458);
    ReadU32(mat + kMatM45c, &m45c);
    ReadU32(mat + kMatM468, &m468);
    char tail[120];
    std::snprintf(tail, sizeof tail, " mkind=%u m458=%u m45c=%u m468=%u slots=%d", static_cast<unsigned>(m460),
                  static_cast<unsigned>(m458), static_cast<unsigned>(m45c), static_cast<unsigned>(m468), slots);
    line += tail;
    Emit(line);
}

} // namespace

void RegisterPass() {
    SotcHooks::Handler resolve;
    resolve.pre = &OnResolve;
    SotcHooks::RegisterHandler("PASS_RESOLVE", resolve);

    SotcHooks::Handler material;
    material.pre = &OnMaterialPre;
    material.post = &OnMaterialPost;
    SotcHooks::RegisterHandler("MATERIAL_LOADED", material);
}

} // namespace SotcProbes
