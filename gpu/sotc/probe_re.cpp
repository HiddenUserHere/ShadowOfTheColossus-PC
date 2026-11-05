// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RE_PROBE=1 (off by default; observe only, nothing is changed). One run confirms the open items of the
// RE notes in docs/re-stutters/native/re-pipelines/. Hooks in scripts/hooks.d/re.py, one group per hook, so a hook
// that does not resolve or is refused turns off only itself. Topics (output "SOTC RE <topic> ...", capped per topic):
//   material      texture-binding.md T7.7, T8.9, A.7, T11.6: material lists, blocks, gates, T# words.
//                 Hooks: RE_MATERIAL_FACTORY (wrap 0xf9390, return value), RE_MATERIAL_LOADER (wrap 0xf9530, rax
//                 taken as the material), RE_MATERIAL_BIND (entry 0xf8c90, first pass bind). Once per material.
//   srt-chunk     texture-binding.md T12.7 item 1, vertex-formats.md T4.10 item 3: the SGPR chunk of 0x862490.
//   srt-mask      vertex-formats.md T4.10 item 2: the mask and the P+0x90.. slots of 0x95320.
//   refresh       texture-binding.md T8.9 item 4, A.7 item 5: T# placement after 0xf8d10 and 0xf9c40 (wrap).
//   skin-*        vertex-formats.md T5.11, T14.8: palette 0x4938e0, queue 0x493bc0, scratch 0x123330 (wrap),
//                 dispatch 0x193810, and the store to P+0x90 (site 0x10c45f).
//   pass-*        pass-targets.md K6.9 items 2, 3, 4, 5, 7: Flip state 0x90860 (+ manager M and the Neo flag),
//                 mode change 0xa1cd0, table 0x9fba0, MRT bind 0x92140. Items 1 and 6 are not hooked.
// The port-side SRT walk of the first 64 VS and 64 PS programs is logged by the recompiler (same switch), topic walker.
// Guest reads go through the backing view with a null and mapped-region guard (probe_pass.cpp pattern).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "hooks.h"
#include "probes.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
// runtime_sotc.c: 1 when the port runs as a PS4 Pro (SOTC_NEO).
int runtime_sotc_neo(void);
}

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

enum Topic : int {
    kMaterial,
    kSrtChunk,
    kSrtMask,
    kRefresh,
    kSkinPalette,
    kSkinQueue,
    kSkinScratch,
    kSkinDispatch,
    kSkinStore,
    kPassFlip,
    kPassMode,
    kPassTable,
    kPassMrt,
    kSkinVSharp,
    kPassView,
    kDecalCb,
    kTopicCount
};
constexpr const char* kTopicName[kTopicCount] = {
    "material",      "srt-chunk",     "srt-mask",     "refresh",     "skin-palette",
    "skin-queue",    "skin-scratch",  "skin-dispatch", "skin-store", "pass-flip",
    "pass-mode",     "pass-table",    "pass-mrt",     "skin-vsharp", "pass-view",
    "decal-cb",
};
// Lines per topic; the material topic carries the most (one block of lines per material).
constexpr int kTopicCap[kTopicCount] = {1500, 300, 300, 300, 300, 300, 300, 300, 300, 300, 300, 300, 300, 300, 300, 300};
constexpr int kDecalDraws = 8; // draws logged after each decal MRT bind (0xc28c0 entry)

constexpr size_t kSeenCap = size_t{1} << 16; // distinct keys per topic
constexpr int kMaxEntries = 256;             // list entries and T# candidates examined per block
constexpr int kEntryLinesPerList = 32;       // material-entry lines per list
constexpr int kMatDepth = 16;                // open wrap calls kept per thread
// Manager of the video out: the owner O is the global at image + kOwnerGlobal, M = *(O + 0x80) (pass-targets.md K6.3).
constexpr uintptr_t kOwnerGlobal = 0x2ed3670;
[[maybe_unused]] constexpr uintptr_t kManagerOwnerOffset = 0x80; // sotc: wrong (see ManagerM)

// Material (texture-binding.md T1.3, T8.1, A.2). A MatTexBlock: payload +0x00, data +0x08, size +0x38, count +0x3c.
constexpr uintptr_t kMatMask = 0x468;
constexpr uintptr_t kMatBlobABegin = 0x470, kMatBlobAEnd = 0x478;
constexpr uintptr_t kMatBlobBBegin = 0x4b0, kMatBlobBEnd = 0x4b8;
constexpr uintptr_t kMatList1Begin = 0x490, kMatList1End = 0x498;
constexpr uintptr_t kMatList2Begin = 0x4d0, kMatList2End = 0x4d8;
constexpr uintptr_t kMatBlock1 = 0x578, kMatBlock2 = 0x5b8;
constexpr uintptr_t kBlockPayload = 0x00, kBlockData = 0x08, kBlockSize = 0x38, kBlockCount = 0x3c;
constexpr uintptr_t kPayloadT = 0xc4; // CTexture T# words, +0xc4..+0xdc (seven dwords)
constexpr uintptr_t kResPath = 0x70;  // resource object: path text (T1.2 step 3)

// ---------------------------------------------------------------------------------------------------------------
// Guest memory and strings.

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

// Zero when unreadable: the logs print what the game holds, and a zero marks the read as failed.
uint64_t U64At(uintptr_t a) {
    uint64_t v = 0;
    ReadGuest(a, &v, sizeof v);
    return v;
}

uint32_t U32At(uintptr_t a) {
    uint32_t v = 0;
    ReadGuest(a, &v, sizeof v);
    return v;
}

// Copies a NUL-terminated guest string (always terminated), bounded to the mapped region.
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
    const size_t limit = cap - 1 < avail ? cap - 1 : avail;
    size_t i = 0;
    for (; i < limit && p[i] != 0; ++i) {
        out[i] = static_cast<char>(p[i]);
    }
    out[i] = '\0';
    return true;
}

bool Printable(const char* s) {
    for (; *s; ++s) {
        if (*s < 0x20 || *s > 0x7e) {
            return false;
        }
    }
    return true;
}

// Path text of a resource object (T1.2): the C string its +0x70 pointer reaches, else the bytes at +0x70.
// kind says which one was used ("ptr", "inl") or "none" when neither is printable text.
void PathOf(uintptr_t res, char* out, size_t cap, const char** kind) {
    out[0] = '\0';
    *kind = "none";
    if (!res) {
        return;
    }
    const uint64_t ptr = U64At(res + kResPath);
    if (ptr >= 0x10000 && ReadGuestString(static_cast<uintptr_t>(ptr), out, cap) && out[0] && Printable(out)) {
        *kind = "ptr";
        return;
    }
    if (ReadGuestString(res + kResPath, out, cap) && out[0] && Printable(out)) {
        *kind = "inl";
        return;
    }
    std::snprintf(out, cap, "?");
}

// ---------------------------------------------------------------------------------------------------------------
// Shared state, guarded by g_mutex.

std::mutex g_mutex;
int g_lines[kTopicCount] = {};
uint64_t g_dropped[kTopicCount] = {};
std::unordered_set<uint64_t> g_seen[kTopicCount];
uint64_t g_flip_calls = 0; // 0x90860 calls (the frame counter of the pass topics)
bool g_flip_started = false;

constexpr uint64_t kHashSeed = 0xcbf29ce484222325ULL;
uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 0x100000001b3ULL;
    return h;
}

// Prints one line unless the topic is at its cap (then one "capped" line, then a count). Caller holds g_mutex.
void Emit(Topic t, const std::string& line) {
    if (g_lines[t] < kTopicCap[t]) {
        std::fprintf(stderr, "%s\n", line.c_str());
        ++g_lines[t];
    } else if (g_dropped[t]++ == 0) {
        std::fprintf(stderr, "SOTC RE %s capped at %d lines, later lines dropped\n", kTopicName[t], kTopicCap[t]);
    }
}

// True the first time key is seen in topic t (bounded set). Caller holds g_mutex.
bool NewKey(Topic t, uint64_t key) {
    if (g_seen[t].size() >= kSeenCap) {
        return false;
    }
    return g_seen[t].insert(key).second;
}

// Formats n floats of guest memory at a into out ("[f0 f1 ...]"). Unreadable memory prints "[?]".
void AppendFloats(std::string& out, uintptr_t a, int n) {
    float f[24] = {};
    char buf[32];
    if (n > 24 || !ReadGuest(a, f, sizeof(float) * static_cast<size_t>(n))) {
        out += "[?]";
        return;
    }
    out += "[";
    for (int i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%s%.4g", i ? " " : "", static_cast<double>(f[i]));
        out += buf;
    }
    out += "]";
}

// Formats n dwords of guest memory at a as hex ("[w0 w1 ...]").
// sotc: hex text of a value (the "0x" prefix is in the caller's literal).
std::string Hx(unsigned long long v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", v);
    return buf;
}

void AppendDwords(std::string& out, uintptr_t a, int n) {
    uint32_t w[16] = {};
    char buf[16];
    if (n > 16 || !ReadGuest(a, w, sizeof(uint32_t) * static_cast<size_t>(n))) {
        out += "[?]";
        return;
    }
    out += "[";
    for (int i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%s%08x", i ? " " : "", static_cast<unsigned>(w[i]));
        out += buf;
    }
    out += "]";
}

// ---------------------------------------------------------------------------------------------------------------
// Material topic and the refresh topic.

// T# words of a payload (CTexture+0xc4..+0xdc), or "none".
std::string TWords(uint64_t payload) {
    if (!payload) {
        return "T=none";
    }
    std::string s = "T=";
    AppendDwords(s, static_cast<uintptr_t>(payload + kPayloadT), 7);
    return s;
}

// One material. Caller holds g_mutex. Logs the material line and up to kEntryLinesPerList entries per list.
void DumpMaterial(uintptr_t mat, const char* src) {
    if (!mat || !NewKey(kMaterial, static_cast<uint64_t>(mat))) {
        return;
    }
    const uint32_t mask = U32At(mat + kMatMask);
    const uint32_t gate1 = U32At(mat + kMatBlock1 + kBlockSize);
    const uint32_t count1 = U32At(mat + kMatBlock1 + kBlockCount);
    const uint32_t gate2 = U32At(mat + kMatBlock2 + kBlockSize);
    const uint32_t count2 = U32At(mat + kMatBlock2 + kBlockCount);
    const int64_t s1 = static_cast<int64_t>(gate1) - 32 * static_cast<int64_t>(count1);
    const int64_t s2 = static_cast<int64_t>(gate2) - 32 * static_cast<int64_t>(count2);
    const uint64_t blob_a = U64At(mat + kMatBlobAEnd) - U64At(mat + kMatBlobABegin);
    const uint64_t blob_b = U64At(mat + kMatBlobBEnd) - U64At(mat + kMatBlobBBegin);
    const uint64_t list1 = (U64At(mat + kMatList1End) - U64At(mat + kMatList1Begin)) / 16;
    const uint64_t list2 = (U64At(mat + kMatList2End) - U64At(mat + kMatList2Begin)) / 16;
    char head[512];
    std::snprintf(head, sizeof head,
                  "SOTC RE material src=%s mat=0x%llx mask=0x%x gate1=%u count1=%u S1=%lld gate2=%u count2=%u "
                  "S2=%lld blobA=%llu blobB=%llu S1mod16=%lld S2mod16=%lld list1=%llu list2=%llu",
                  src, static_cast<unsigned long long>(mat), static_cast<unsigned>(mask),
                  static_cast<unsigned>(gate1), static_cast<unsigned>(count1), static_cast<long long>(s1),
                  static_cast<unsigned>(gate2), static_cast<unsigned>(count2), static_cast<long long>(s2),
                  static_cast<unsigned long long>(blob_a), static_cast<unsigned long long>(blob_b),
                  static_cast<long long>(((s1 % 16) + 16) % 16), static_cast<long long>(((s2 % 16) + 16) % 16),
                  static_cast<unsigned long long>(list1), static_cast<unsigned long long>(list2));
    Emit(kMaterial, head);

    for (int list = 1; list <= 2; ++list) {
        const uintptr_t begin_off = list == 1 ? kMatList1Begin : kMatList2Begin;
        const uintptr_t block = mat + (list == 1 ? kMatBlock1 : kMatBlock2);
        const uintptr_t begin = static_cast<uintptr_t>(U64At(mat + begin_off));
        const uint64_t n = (U64At(mat + begin_off + 8) - static_cast<uint64_t>(begin)) / 16;
        const uint64_t payloads = U64At(block + kBlockPayload);
        for (uint64_t i = 0; i < n && i < static_cast<uint64_t>(kEntryLinesPerList); ++i) {
            const uintptr_t entry = begin + static_cast<uintptr_t>(16 * i);
            const uint64_t res = U64At(entry);
            char path[160];
            const char* kind = "none";
            PathOf(static_cast<uintptr_t>(res), path, sizeof path, &kind);
            const uint64_t payload = payloads ? U64At(static_cast<uintptr_t>(payloads + 8 * i)) : 0;
            char line[400];
            std::snprintf(line, sizeof line, "SOTC RE material-entry mat=0x%llx list=%d i=%llu res=0x%llx path=\"%s\" "
                          "pathkind=%s payload=0x%llx %s",
                          static_cast<unsigned long long>(mat), list, static_cast<unsigned long long>(i),
                          static_cast<unsigned long long>(res), path, kind,
                          static_cast<unsigned long long>(payload), TWords(payload).c_str());
            Emit(kMaterial, line);
        }
    }
}

// Refresh check (T8.9 item 4, A.7 item 5). For each block with entries: the candidate T# addresses
// A = data+S+32i, B = data+round16(S)+32i and C = data+round16(S)-32*count+32i, and how many of the count T#s
// (the first 4 dwords of each payload's T#) each candidate holds. Caller holds g_mutex.
void LogRefresh(uintptr_t mat, const char* site) {
    for (int blk = 1; blk <= 2; ++blk) {
        const uintptr_t block = mat + (blk == 1 ? kMatBlock1 : kMatBlock2);
        const uint32_t gate = U32At(block + kBlockSize);
        const uint32_t count = U32At(block + kBlockCount);
        if (count == 0 || count > static_cast<uint32_t>(kMaxEntries)) {
            continue;
        }
        const int64_t s = static_cast<int64_t>(gate) - 32 * static_cast<int64_t>(count);
        if (s < 0) {
            continue;
        }
        const uint64_t data = U64At(block + kBlockData);
        const uint64_t payloads = U64At(block + kBlockPayload);
        const uint64_t r16 = (static_cast<uint64_t>(s) + 15) & ~uint64_t{15};
        uint32_t match[3] = {0, 0, 0};
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t payload = payloads ? U64At(static_cast<uintptr_t>(payloads + 8 * i)) : 0;
            uint32_t t[4] = {};
            if (!payload || !ReadGuest(static_cast<uintptr_t>(payload + kPayloadT), t, sizeof t)) {
                continue;
            }
            const uint64_t cand[3] = {data + static_cast<uint64_t>(s) + 32 * i, data + r16 + 32 * i,
                                      data + r16 - 32 * uint64_t{count} + 32 * i};
            for (int c = 0; c < 3; ++c) {
                uint32_t w[4] = {};
                if (data && ReadGuest(static_cast<uintptr_t>(cand[c]), w, sizeof w) && std::memcmp(w, t, sizeof w) == 0) {
                    ++match[c];
                }
            }
        }
        uint64_t key = Mix(Mix(kHashSeed, static_cast<uint64_t>(mat)), (uint64_t{gate} << 32) | count);
        key = Mix(Mix(Mix(key, match[0]), match[1]), match[2]) ^ static_cast<uint64_t>(blk);
        if (!NewKey(kRefresh, key)) {
            continue;
        }
        char line[400];
        std::snprintf(line, sizeof line,
                      "SOTC RE refresh site=%s mat=0x%llx blk=%d gate=%u count=%u S=%lld S16=%llu data=0x%llx "
                      "A=%u/%u B=%u/%u C=%u/%u",
                      site, static_cast<unsigned long long>(mat), blk, static_cast<unsigned>(gate),
                      static_cast<unsigned>(count), static_cast<long long>(s),
                      static_cast<unsigned long long>(r16), static_cast<unsigned long long>(data),
                      static_cast<unsigned>(match[0]), static_cast<unsigned>(count), static_cast<unsigned>(match[1]),
                      static_cast<unsigned>(count), static_cast<unsigned>(match[2]), static_cast<unsigned>(count));
        Emit(kRefresh, line);
    }
}

// Per-thread stack of the material pointers of open refresh calls (the pre phase pushes, the post phase pops).
struct MatStack {
    uintptr_t mat[kMatDepth] = {};
    int depth = 0;
};
thread_local MatStack t_refresh;

// Per-thread stack of (obj, scratch) of open 0x123330 calls.
struct PairStack {
    uintptr_t a[kMatDepth] = {};
    uintptr_t b[kMatDepth] = {};
    int depth = 0;
};
thread_local PairStack t_scratch;

HookAction PreNone(GuestRegs&, void*) {
    return HookAction::Continue;
}

// RE_MATERIAL_FACTORY: wrap of 0xf9390. The factory returns the material (rbx) when the parse succeeded.
void PostFactory(GuestRegs& regs, void*) {
    const auto mat = static_cast<uintptr_t>(regs.rax);
    std::lock_guard<std::mutex> lock(g_mutex);
    DumpMaterial(mat, "factory");
}

// RE_MATERIAL_LOADER: wrap of 0xf9530. The return value is taken as the material; check the logged gates.
void PostLoader(GuestRegs& regs, void*) {
    const auto mat = static_cast<uintptr_t>(regs.rax);
    std::lock_guard<std::mutex> lock(g_mutex);
    DumpMaterial(mat, "loader-rax");
}

// RE_MATERIAL_BIND: entry of 0xf8c90 (material, pass). The first bind of each material.
HookAction OnBind(GuestRegs& regs, void*) {
    const auto mat = static_cast<uintptr_t>(regs.rdi);
    std::lock_guard<std::mutex> lock(g_mutex);
    DumpMaterial(mat, "bind");
    return HookAction::Continue;
}

HookAction PreRefresh(GuestRegs& regs, void*) {
    if (t_refresh.depth < kMatDepth) {
        t_refresh.mat[t_refresh.depth] = static_cast<uintptr_t>(regs.rdi);
    }
    ++t_refresh.depth;
    return HookAction::Continue;
}

void PostRefresh(GuestRegs&, void* user) {
    --t_refresh.depth;
    if (t_refresh.depth < 0 || t_refresh.depth >= kMatDepth) {
        return;
    }
    const uintptr_t mat = t_refresh.mat[t_refresh.depth];
    if (!mat) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRefresh(mat, static_cast<const char*>(user));
}

// ---------------------------------------------------------------------------------------------------------------
// SRT chunk and mask (texture-binding.md T12.7, vertex-formats.md T4.10).

// RE_SRT_CHUNK: entry of 0x862490 (O, stage, src, n). Layout index 0 = CS, 1 = PS, 2 = VS (T4.5 step 3).
HookAction OnSrtChunk(GuestRegs& regs, void*) {
    const auto o = static_cast<uintptr_t>(regs.rdi);
    const auto s = static_cast<uint32_t>(regs.rsi);
    const auto src = static_cast<uintptr_t>(regs.rdx);
    if (s > 2) {
        return HookAction::Continue;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t layout = U64At(o + 0xc180 + 8 * s);
    uint16_t lay = 0;
    if (layout) {
        ReadGuest(static_cast<uintptr_t>(layout + 0x10), &lay, sizeof lay);
    }
    const uint64_t chunk = U64At(src);
    uint64_t q[8] = {};
    for (int k = 0; k < 8 && chunk; ++k) {
        q[k] = U64At(static_cast<uintptr_t>(chunk + 8 * static_cast<uint64_t>(k)));
    }
    uint64_t key = Mix(kHashSeed, s);
    key = Mix(key, lay);
    for (int k = 0; k < 8; ++k) {
        key = Mix(key, q[k]);
    }
    if (!NewKey(kSrtChunk, key)) {
        return HookAction::Continue;
    }
    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC RE srt-chunk layout=%u lay_u16=0x%04x sgpr=%u count=%u src=0x%llx chunk=0x%llx "
                  "q=0x%016llx 0x%016llx 0x%016llx 0x%016llx 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  static_cast<unsigned>(s), static_cast<unsigned>(lay), static_cast<unsigned>(lay & 0xff),
                  static_cast<unsigned>(lay >> 8), static_cast<unsigned long long>(src),
                  static_cast<unsigned long long>(chunk), static_cast<unsigned long long>(q[0]),
                  static_cast<unsigned long long>(q[1]), static_cast<unsigned long long>(q[2]),
                  static_cast<unsigned long long>(q[3]), static_cast<unsigned long long>(q[4]),
                  static_cast<unsigned long long>(q[5]), static_cast<unsigned long long>(q[6]),
                  static_cast<unsigned long long>(q[7]));
    Emit(kSrtChunk, line);
    return HookAction::Continue;
}

// RE_SRT_MASK: entry of 0x95320 (mask, P+0x90 slot array). The slots are P[0..9] (P+0x90..+0xd8).
HookAction OnSrtMask(GuestRegs& regs, void*) {
    const auto mask = static_cast<uint32_t>(regs.rdi);
    const auto base = static_cast<uintptr_t>(regs.rsi);
    std::lock_guard<std::mutex> lock(g_mutex);
    uint64_t q[10] = {};
    uint64_t key = Mix(kHashSeed, mask);
    for (int k = 0; k < 10; ++k) {
        q[k] = U64At(base + 8 * static_cast<uintptr_t>(k));
        key = Mix(key, q[k]);
    }
    if (!NewKey(kSrtMask, key)) {
        return HookAction::Continue;
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "SOTC RE srt-mask mask=0x%x base=0x%llx slots=[0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx "
                  "0x%llx 0x%llx 0x%llx]",
                  static_cast<unsigned>(mask), static_cast<unsigned long long>(base),
                  static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
                  static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]),
                  static_cast<unsigned long long>(q[4]), static_cast<unsigned long long>(q[5]),
                  static_cast<unsigned long long>(q[6]), static_cast<unsigned long long>(q[7]),
                  static_cast<unsigned long long>(q[8]), static_cast<unsigned long long>(q[9]));
    Emit(kSrtMask, line);
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------------
// Skinning (vertex-formats.md T5.11, T14.8).

// RE_SKIN_PALETTE: entry of 0x4938e0 (manager/obj in rdi, record in rsi). The record fields are the spec's
// rec+0x10 (count), rec+0x14 (48 bytes) and rec+0x44 (48 bytes); the identity of rsi as the record is inferred.
HookAction OnSkinPalette(GuestRegs& regs, void*) {
    const auto obj = static_cast<uintptr_t>(regs.rdi);
    const auto rec = static_cast<uintptr_t>(regs.rsi);
    std::lock_guard<std::mutex> lock(g_mutex);
    float a[12] = {};
    float b[12] = {};
    const bool ra = ReadGuest(rec + 0x14, a, sizeof a);
    const bool rb = ReadGuest(rec + 0x44, b, sizeof b);
    uint64_t key = Mix(kHashSeed, static_cast<uint64_t>(obj));
    for (int k = 0; k < 12; ++k) {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, &a[k], sizeof x);
        std::memcpy(&y, &b[k], sizeof y);
        key = Mix(Mix(key, x), y);
    }
    if (!NewKey(kSkinPalette, key)) {
        return HookAction::Continue;
    }
    std::string line = "SOTC RE skin-palette obj=0x" + Hx(static_cast<unsigned long long>(obj)) + " rec=0x" +
                       Hx(static_cast<unsigned long long>(rec));
    char buf[160];
    std::snprintf(buf, sizeof buf, " mgr20=%d rec10=%u", static_cast<int>(U32At(obj + 0x20)),
                  static_cast<unsigned>(U32At(rec + 0x10)));
    line += buf;
    line += " rec14=";
    line += ra ? "" : "?";
    if (ra) {
        AppendFloats(line, rec + 0x14, 12);
    }
    line += " rec44=";
    if (rb) {
        AppendFloats(line, rec + 0x44, 12);
    } else {
        line += "[?]";
    }
    Emit(kSkinPalette, line);
    return HookAction::Continue;
}

// RE_SKIN_QUEUE: entry of 0x493bc0. The arguments and the return address (at [rsp] on entry).
HookAction OnSkinQueue(GuestRegs& regs, void*) {
    uintptr_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof ret);
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC RE skin-queue ret=0x%llx rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx r8=0x%llx r9=0x%llx",
                  static_cast<unsigned long long>(ret), static_cast<unsigned long long>(regs.rdi),
                  static_cast<unsigned long long>(regs.rsi), static_cast<unsigned long long>(regs.rdx),
                  static_cast<unsigned long long>(regs.rcx), static_cast<unsigned long long>(regs.r8),
                  static_cast<unsigned long long>(regs.r9));
    Emit(kSkinQueue, line);
    return HookAction::Continue;
}

// RE_SKIN_SCRATCH: wrap of 0x123330 (obj in rdi, scratch in rdx). Pre saves both; post reads the palette.
HookAction PreScratch(GuestRegs& regs, void*) {
    if (t_scratch.depth < kMatDepth) {
        t_scratch.a[t_scratch.depth] = static_cast<uintptr_t>(regs.rdi);
        t_scratch.b[t_scratch.depth] = static_cast<uintptr_t>(regs.rdx);
    }
    ++t_scratch.depth;
    return HookAction::Continue;
}

void PostScratch(GuestRegs&, void*) {
    --t_scratch.depth;
    if (t_scratch.depth < 0 || t_scratch.depth >= kMatDepth) {
        return;
    }
    const uintptr_t obj = t_scratch.a[t_scratch.depth];
    const uintptr_t scratch = t_scratch.b[t_scratch.depth];
    if (!obj) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t head = U64At(obj + 0x100);
    const uint32_t count = head ? U32At(static_cast<uintptr_t>(head + 0x18)) : 0;
    const uint32_t dw0 = U32At(scratch);
    const uint32_t dw1 = U32At(scratch + 4);
    const uint64_t nodes = head ? U64At(static_cast<uintptr_t>(head + 0x20)) : 0;
    float node0[12] = {};
    const bool ok = nodes && ReadGuest(static_cast<uintptr_t>(nodes), node0, sizeof node0);
    uint64_t key = Mix(Mix(Mix(kHashSeed, static_cast<uint64_t>(obj)), count), (uint64_t{dw0} << 32) | dw1);
    for (int k = 0; k < 12 && ok; ++k) {
        uint32_t x = 0;
        std::memcpy(&x, &node0[k], sizeof x);
        key = Mix(key, x);
    }
    if (!NewKey(kSkinScratch, key)) {
        return;
    }
    std::string line = "SOTC RE skin-scratch obj=0x" + Hx(static_cast<unsigned long long>(obj)) +
                       " head=0x" + Hx(static_cast<unsigned long long>(head)) + " count=" +
                       std::to_string(count) + " scratch_dw0=" + std::to_string(dw0) + " scratch_dw1=" +
                       std::to_string(dw1) + " node0=";
    if (ok) {
        AppendFloats(line, static_cast<uintptr_t>(nodes), 12);
    } else {
        line += "[?]";
    }
    Emit(kSkinScratch, line);
}

// RE_SKIN_DISPATCH: entry of 0x193810 (rec in rdi, ctx in rdx, palette pointer in rcx). Output V#s at out+0xe8 +
// p*0x28, out+0x138 and out+0x160 (out = *(rec+0x40)); p = *(*(ctx+0x10)+0xd70) & 1 (vertex-formats.md T14.5).
// The five input V#s need the mesh stream, which is reached through a guest call (0x14d7b0); not read here.
HookAction OnSkinDispatch(GuestRegs& regs, void*) {
    const auto rec = static_cast<uintptr_t>(regs.rdi);
    const auto ctx = static_cast<uintptr_t>(regs.rdx);
    const auto pal = static_cast<uintptr_t>(regs.rcx);
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t res = U64At(rec + 0x20);
    const uint64_t out = U64At(rec + 0x40);
    const uint64_t pctx = ctx ? U64At(ctx + 0x10) : 0;
    const uint32_t p = pctx ? U32At(static_cast<uintptr_t>(pctx + 0xd70)) & 1u : 0u;
    const uint64_t base = U64At(pal);
    uint32_t v0[4] = {}, v1[4] = {}, v2[4] = {};
    const uintptr_t o = static_cast<uintptr_t>(out);
    if (out) {
        ReadGuest(o + 0xe8 + 0x28 * p, v0, sizeof v0);
        ReadGuest(o + 0x138, v1, sizeof v1);
        ReadGuest(o + 0x160, v2, sizeof v2);
    }
    uint64_t key = Mix(Mix(kHashSeed, rec), base);
    for (int k = 0; k < 4; ++k) {
        key = Mix(Mix(Mix(key, v0[k]), v1[k]), v2[k]);
    }
    if (!NewKey(kSkinDispatch, key)) {
        return HookAction::Continue;
    }
    std::string line = "SOTC RE skin-dispatch rec=0x" + Hx(static_cast<unsigned long long>(rec)) +
                       " res=0x" + Hx(static_cast<unsigned long long>(res)) + " out=0x" +
                       Hx(static_cast<unsigned long long>(out)) + " p=" + std::to_string(p) +
                       " rsi=0x" + Hx(static_cast<unsigned long long>(regs.rsi)) + " ctx=0x" +
                       Hx(static_cast<unsigned long long>(ctx)) + " pal=0x" +
                       Hx(static_cast<unsigned long long>(pal)) + " pal_base=0x" +
                       Hx(static_cast<unsigned long long>(base)) + " pal_v=";
    AppendDwords(line, pal, 4);
    line += " out_v0=";
    AppendDwords(line, o + 0xe8 + 0x28 * p, 4);
    line += " out_v1=";
    AppendDwords(line, o + 0x138, 4);
    line += " out_v2=";
    AppendDwords(line, o + 0x160, 4);
    Emit(kSkinDispatch, line);
    return HookAction::Continue;
}

// RE_SKIN_STORE: site 0x10c45f, the store of the table pointer (holder+0x60) to P+0x90 in 0x10c2e0 (T14.8 item 5).
// P is r14, the holder rbx, the value rax. The 16-byte V# at table+0x10 is logged with the parity from P+0x10.
HookAction OnSkinStore(GuestRegs& regs, void*) {
    const auto p_obj = static_cast<uintptr_t>(regs.r14);
    const auto holder = static_cast<uintptr_t>(regs.rbx);
    const auto table = static_cast<uintptr_t>(regs.rax);
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t pp = U64At(p_obj + 0x10);
    const uint32_t par = pp ? U32At(static_cast<uintptr_t>(pp + 0xd70)) & 1u : 0u;
    uint32_t v[4] = {};
    if (table) {
        ReadGuest(table + 0x10, v, sizeof v);
    }
    uint64_t key = Mix(Mix(Mix(kHashSeed, p_obj), holder), table);
    for (int k = 0; k < 4; ++k) {
        key = Mix(key, v[k]);
    }
    if (!NewKey(kSkinStore, key)) {
        return HookAction::Continue;
    }
    std::string line = "SOTC RE skin-store P=0x" + Hx(static_cast<unsigned long long>(p_obj)) +
                       " holder=0x" + Hx(static_cast<unsigned long long>(holder)) + " table=0x" +
                       Hx(static_cast<unsigned long long>(table)) + " parity_P=" + std::to_string(par) +
                       " table_v=";
    AppendDwords(line, table + 0x10, 4);
    Emit(kSkinStore, line);
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------------
// Pass topic (pass-targets.md K6.9).

// The manager M of the video out: O = the global at the image base + kOwnerGlobal, M = *(O + 0x80). 0 when unknown.
uint64_t ManagerM() {
    const auto base = static_cast<uint64_t>(SotcHooks::ImageBase());
    if (!base) {
        return 0;
    }
    const uint64_t owner = U64At(static_cast<uintptr_t>(base + kOwnerGlobal));
    // sotc: run of 4 Nov 2026: *(owner + 0x80) was 0 in every use; 0x9fba0 / 0xa1cd0 get the global itself.
    return owner;
}

// RE_PASS_FLIP: entry of 0x90860 (R in rdi, param_2 in rsi, S = param_2[9]). Items 2 and 3 are logged once, at the
// first call, with the Neo flag and the manager's +0x2288 and +0x1e80 fields. Item 5 logs S, S+0x4b8 and S+0x4c0.
HookAction OnPassFlip(GuestRegs& regs, void*) {
    const auto r = static_cast<uintptr_t>(regs.rdi);
    const auto param2 = static_cast<uintptr_t>(regs.rsi);
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t call = ++g_flip_calls;
    if (!g_flip_started) {
        g_flip_started = true;
        const uint64_t m = ManagerM();
        const uint64_t f2288 = m ? U64At(static_cast<uintptr_t>(m + 0x2288)) : 0;
        const uint32_t cnt = f2288 ? U32At(static_cast<uintptr_t>(f2288 + 0x1a8)) : 0;
        char line[320];
        std::snprintf(line, sizeof line,
                      "SOTC RE pass-flip first-call neo=%d M=0x%llx M2288=0x%llx count1a8=%u M1e80=0x%llx",
                      runtime_sotc_neo(), static_cast<unsigned long long>(m),
                      static_cast<unsigned long long>(f2288), static_cast<unsigned>(cnt),
                      static_cast<unsigned long long>(m ? U64At(static_cast<uintptr_t>(m + 0x1e80)) : 0));
        Emit(kPassFlip, line);
    }
    const uint64_t s = U64At(param2 + 9 * 8);
    const uint32_t v4b8 = s ? U32At(static_cast<uintptr_t>(s + 0x4b8)) : 0;
    const uint32_t v4c0 = s ? U32At(static_cast<uintptr_t>(s + 0x4c0)) : 0;
    const uint64_t key = Mix(Mix(Mix(kHashSeed, s), v4b8), v4c0);
    if (NewKey(kPassFlip, key)) {
        char line[320];
        std::snprintf(line, sizeof line,
                      "SOTC RE pass-flip call=%llu R=0x%llx param2=0x%llx S=0x%llx S4b8=%u S4c0=%u",
                      static_cast<unsigned long long>(call), static_cast<unsigned long long>(r),
                      static_cast<unsigned long long>(param2), static_cast<unsigned long long>(s),
                      static_cast<unsigned>(v4b8), static_cast<unsigned>(v4c0));
        Emit(kPassFlip, line);
    }
    return HookAction::Continue;
}

// RE_PASS_MODE: entry of 0xa1cd0 (M in rdi, new mode in esi). Logs each change of the mode word at +0x70.
HookAction OnPassMode(GuestRegs& regs, void*) {
    const auto m = static_cast<uintptr_t>(regs.rdi);
    const auto next = static_cast<uint32_t>(regs.rsi);
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t old = U32At(m + 0x70);
    if (old == next) {
        return HookAction::Continue;
    }
    char line[200];
    std::snprintf(line, sizeof line, "SOTC RE pass-mode M=0x%llx old=%u new=%u flip_call=%llu",
                  static_cast<unsigned long long>(m), static_cast<unsigned>(old), static_cast<unsigned>(next),
                  static_cast<unsigned long long>(g_flip_calls));
    Emit(kPassMode, line);
    return HookAction::Continue;
}

// RE_PASS_TABLE: entry of 0x9fba0 (M in rdi). Dumps the M+0xb40+i and M+0x1680+i words for the set sources of K6.9
// item 4. Logged when the values change.
HookAction OnPassTable(GuestRegs& regs, void*) {
    const auto m = static_cast<uintptr_t>(regs.rdi);
    static constexpr uint32_t kIdx[10] = {0x08, 0x38, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0xf8, 0x380};
    std::lock_guard<std::mutex> lock(g_mutex);
    uint64_t vals[20] = {};
    uint64_t key = Mix(kHashSeed, static_cast<uint64_t>(m));
    for (int k = 0; k < 10; ++k) {
        vals[2 * k] = U64At(m + 0xb40 + kIdx[k]);
        vals[2 * k + 1] = U64At(m + 0x1680 + kIdx[k]);
        key = Mix(Mix(key, vals[2 * k]), vals[2 * k + 1]);
    }
    if (!NewKey(kPassTable, key)) {
        return HookAction::Continue;
    }
    std::string line = "SOTC RE pass-table M=0x" + Hx(static_cast<unsigned long long>(m)) + " neo=" +
                       std::to_string(runtime_sotc_neo());
    char buf[64];
    for (int k = 0; k < 10; ++k) {
        std::snprintf(buf, sizeof buf, " [%02x]=0x%llx/0x%llx", static_cast<unsigned>(kIdx[k]),
                      static_cast<unsigned long long>(vals[2 * k]), static_cast<unsigned long long>(vals[2 * k + 1]));
        line += buf;
    }
    Emit(kPassTable, line);
    return HookAction::Continue;
}

// RE_PASS_MRT: entry of 0x92140 (P in rdi, P[0] and P[1] the slot pointers). Compared with M+0x60 and M+0x58.
HookAction OnPassMrt(GuestRegs& regs, void*) {
    const auto p = static_cast<uintptr_t>(regs.rdi);
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t p0 = U64At(p);
    const uint64_t p1 = U64At(p + 8);
    const uint64_t key = Mix(Mix(Mix(kHashSeed, static_cast<uint64_t>(p)), p0), p1);
    if (!NewKey(kPassMrt, key)) {
        return HookAction::Continue;
    }
    const uint64_t m = ManagerM();
    const uint64_t m60 = m ? U64At(static_cast<uintptr_t>(m + 0x60)) : 0;
    const uint64_t m58 = m ? U64At(static_cast<uintptr_t>(m + 0x58)) : 0;
    char line[320];
    std::snprintf(line, sizeof line,
                  "SOTC RE pass-mrt P=0x%llx slot0=0x%llx slot1=0x%llx M=0x%llx M60=0x%llx M58=0x%llx eq0=%d eq1=%d",
                  static_cast<unsigned long long>(p), static_cast<unsigned long long>(p0),
                  static_cast<unsigned long long>(p1), static_cast<unsigned long long>(m),
                  static_cast<unsigned long long>(m60), static_cast<unsigned long long>(m58),
                  p0 == m60 ? 1 : 0, p1 == m58 ? 1 : 0);
    Emit(kPassMrt, line);
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------------
// Skin V# writers (vertex-formats.md T5.5, T14.8). 0x85e8e0 (base 0x11), 0x85e9a0 (base 0x21) and 0x85e820 (base 0x41)
// share one body: entry args rdi = cmd, esi = k, edx = count, rcx = src (16-byte V#s). The table index of V# i is
// k + base + i. Their immediate callers are mixed (0xdc740, 0xdcf40, 0x106950, 0x10c8c0, 0x10cdb0, 0x1122e0,
// 0x1375e0, 0xa7470/0xa7310 for the outputs and extras), so every call is logged with its caller, deduplicated by
// (index, V# words).
HookAction OnVSharp(GuestRegs& regs, void* user) {
    const auto base = static_cast<int64_t>(reinterpret_cast<uintptr_t>(user));
    const auto k = static_cast<int32_t>(regs.rsi);
    const auto count = static_cast<int32_t>(regs.rdx);
    const auto src = static_cast<uintptr_t>(regs.rcx);
    uintptr_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof ret);
    if (count <= 0 || count > 8 || !src) {
        return HookAction::Continue;
    }
    const auto image = static_cast<uint64_t>(SotcHooks::ImageBase());
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int32_t i = 0; i < count; ++i) {
        const int64_t idx = static_cast<int64_t>(k) + base + i;
        uint32_t w[4] = {};
        ReadGuest(src + 16 * static_cast<uintptr_t>(i), w, sizeof w);
        uint64_t key = Mix(Mix(kHashSeed, static_cast<uint64_t>(idx)), static_cast<uint64_t>(base));
        for (int j = 0; j < 4; ++j) {
            key = Mix(key, w[j]);
        }
        if (!NewKey(kSkinVSharp, key)) {
            continue;
        }
        std::string line = "SOTC RE skin-vsharp hook=0x" +
                           Hx(static_cast<unsigned long long>(base)) + " idx=" + std::to_string(idx) +
                           " words=";
        AppendDwords(line, src + 16 * static_cast<uintptr_t>(i), 4);
        char tail[96];
        if (image && ret >= image && ret - image < (uint64_t{1} << 30)) {
            std::snprintf(tail, sizeof tail, " ret=eb:0x%llx", static_cast<unsigned long long>(ret - image));
        } else {
            std::snprintf(tail, sizeof tail, " ret=abs:0x%llx", static_cast<unsigned long long>(ret));
        }
        line += tail;
        Emit(kSkinVSharp, line);
    }
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------------
// K6.9 item 1: the first frame's view. F = rdi of 0xcefa0 (EndFrameViewParams); V = *(*(F+0x40)+0xd98) (K6.3),
// compared with the manager M.
HookAction OnPassView(GuestRegs& regs, void*) {
    const auto f = static_cast<uintptr_t>(regs.rdi);
    std::lock_guard<std::mutex> lock(g_mutex);
    static bool done = false;
    if (done) {
        return HookAction::Continue;
    }
    done = true;
    const uint64_t f40 = U64At(f + 0x40);
    const uint64_t v = f40 ? U64At(static_cast<uintptr_t>(f40 + 0xd98)) : 0;
    const uint64_t m = ManagerM();
    char line[320];
    std::snprintf(line, sizeof line, "SOTC RE pass-view first F=0x%llx F40=0x%llx V=0x%llx M=0x%llx V==M %s",
                  static_cast<unsigned long long>(f), static_cast<unsigned long long>(f40),
                  static_cast<unsigned long long>(v), static_cast<unsigned long long>(m),
                  (v && v == m) ? "yes" : "no");
    Emit(kPassView, line);
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------------
// K6.9 item 6, decal draws. The decal pass binds its MRT in 0xc28c0 (entry arms the logging); the next kDecalDraws
// draws log CB_COLORn_INFO. Arming and draws are counted in g_decal_armed (atomic, so the draw path can test it
// without the mutex).
std::atomic<int> g_decal_armed{0};
uint64_t g_decal_arms = 0;
uint64_t g_decal_draws = 0;

HookAction OnDecalArm(GuestRegs&, void*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_decal_arms;
    g_decal_armed.store(kDecalDraws, std::memory_order_relaxed);
    char line[160];
    std::snprintf(line, sizeof line, "SOTC RE decal-cb armed arm=%llu flip_call=%llu",
                  static_cast<unsigned long long>(g_decal_arms), static_cast<unsigned long long>(g_flip_calls));
    Emit(kDecalCb, line);
    return HookAction::Continue;
}

void Reg(const char* name, SotcHooks::PreHandler pre, SotcHooks::PostHandler post = nullptr, void* user = nullptr) {
    SotcHooks::Handler handler;
    handler.pre = pre;
    handler.post = post;
    handler.user = user;
    SotcHooks::RegisterHandler(name, handler);
}

} // namespace

// Port side (liverpool.cpp SubmitDraw, behind SOTC_RE_PROBE read once). info[cb] = CB_COLORcb_INFO raw word and
// bound_mask bit cb = the target is enabled for this draw. Logs only while a decal MRT bind is armed.
void NoteDecalDraw(const uint32_t info[8], uint32_t bound_mask) {
    if (g_decal_armed.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_decal_armed.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    g_decal_armed.fetch_sub(1, std::memory_order_relaxed);
    const uint64_t draw = ++g_decal_draws;
    std::string line = "SOTC RE decal-cb draw=" + std::to_string(static_cast<unsigned long long>(draw)) +
                       " arm=" + std::to_string(static_cast<unsigned long long>(g_decal_arms)) + " bound=";
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%02x", static_cast<unsigned>(bound_mask & 0xff));
    line += buf;
    for (int cb = 0; cb < 8; ++cb) {
        std::snprintf(buf, sizeof buf, " cb%d=0x%08x", cb, static_cast<unsigned>(info[cb]));
        line += buf;
    }
    Emit(kDecalCb, line);
}

void RegisterRe() {
    // Skin V# writers (one hook each; the base index is the user pointer).
    Reg("RE_SKIN_VSHARP_IN", &OnVSharp, nullptr, reinterpret_cast<void*>(uintptr_t{0x11}));
    Reg("RE_SKIN_VSHARP_OUT", &OnVSharp, nullptr, reinterpret_cast<void*>(uintptr_t{0x21}));
    Reg("RE_SKIN_VSHARP_EXTRA", &OnVSharp, nullptr, reinterpret_cast<void*>(uintptr_t{0x41}));
    // K6.9 item 1 (first view) and item 6 (decal arming).
    Reg("RE_PASS_VIEW", &OnPassView);
    Reg("RE_DECAL_ARM", &OnDecalArm);
    // Material (one group per hook: a refused one turns off only itself).
    Reg("RE_MATERIAL_FACTORY", &PreNone, &PostFactory);
    Reg("RE_MATERIAL_LOADER", &PreNone, &PostLoader);
    Reg("RE_MATERIAL_BIND", &OnBind);
    // SRT chunk and mask.
    Reg("RE_SRT_CHUNK", &OnSrtChunk);
    Reg("RE_SRT_MASK", &OnSrtMask);
    // Refresh (the label is the site, passed as user).
    static const char kSiteF8d10[] = "f8d10";
    static const char kSiteF9c40[] = "f9c40";
    Reg("RE_REFRESH_F8D10", &PreRefresh, &PostRefresh, const_cast<char*>(kSiteF8d10));
    Reg("RE_REFRESH_F9C40", &PreRefresh, &PostRefresh, const_cast<char*>(kSiteF9c40));
    // Skinning.
    Reg("RE_SKIN_PALETTE", &OnSkinPalette);
    Reg("RE_SKIN_QUEUE", &OnSkinQueue);
    Reg("RE_SKIN_SCRATCH", &PreScratch, &PostScratch);
    Reg("RE_SKIN_DISPATCH", &OnSkinDispatch);
    Reg("RE_SKIN_STORE", &OnSkinStore);
    // Pass targets (K6.9).
    Reg("RE_PASS_FLIP", &OnPassFlip);
    Reg("RE_PASS_MODE", &OnPassMode);
    Reg("RE_PASS_TABLE", &OnPassTable);
    Reg("RE_PASS_MRT", &OnPassMrt);
}

} // namespace SotcProbes
