// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_VSIZE_PROBE (off by default, measurement only; no guest write, no protection change).
//
// For every unbounded read-only storage binding (V# size >= 0xFFFF0000) the probe answers, per
// (pgm_hash, binding index):
//   - where the V# words come from: user data SGPRs, or the SRT walker's table. The table is resolved from
//     the provenance the flatten pass records (SotcSrtSource, info.h): the root SGPR pair gives the table
//     pointer, each pointer load of the path is followed in guest memory, and the V# is the dword at
//     table + src_off_dw.
//   - the 16 V# bytes with 64 bytes either side (once per pair), or all user data dwords for SGPR sharps.
//   - the live allocator block holding the base (probe_alloc.cpp node table, SOTC_HOOK_ALLOC_PROBE).
//   - the containing buffer object of the object map (see below), with the remaining extent.
//   - the frame ring of the base (probe_frame_rings.cpp, needs SOTC_FRAME_RINGS=1) and the PC arena
//     relation (SotcPcRing::ArenaRelation).
//   - the dirty bytes the bind's own sync copied (buffer_cache.cpp feeds NoteDirty).
//
// Object map. Two buffer-object creators feed it: 0x8be60 (NATIVE_ZC_CREATE entry, native_hooks.cpp: its
// size is count * stride, computed at entry, and its base is read later) and 0x8c280 (VSIZE_VIEW_SITE, a site
// after the size store; the function leaves through tail jumps, so there is no ret to hook). The base is not
// stored: obj+0x10 is written by the callee, so it is read from guest memory at lookup time. Entries are a
// ring of 65536 (oldest replaced). A lookup walks newest first and reads obj+0x10 of each entry, so it is
// run once per pair and once per report window, not per bind.
//
// Not measured: the guest RIP that writes num_records (item 4 of the task). The write-fault path belongs to the
// page manager; a one-shot protection of the V# page would bypass its bookkeeping, so it is left out.
// Guest reads go through SotcGuest (the unprotected backing view). Logging: stderr.

#include "probe_vsize.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "guest_mem.h"
#include "hooks.h"
#include "pc_ring.h"
#include "probe_frame_rings.h"
#include "probes.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/resource.h"

namespace SotcVsizeProbe {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;
using Shader::SotcSrtSource;

constexpr uint64_t kMask48 = 0xFFFFFFFFFFFFULL;
constexpr uint32_t kUserDataDwords = 16; // NUM_USER_DATA_REGS: flat buffer dwords before the walker's output
constexpr uint32_t kCountMinusOne = 0xffffffffu;
constexpr size_t kMaxPairs = 64;
constexpr size_t kBefore = 64; // bytes before the V#
constexpr size_t kAfter = 64;  // bytes after the V#
constexpr size_t kObjSlots = 65536;
constexpr size_t kMaxCallers = 32;
constexpr uint64_t kOriginBpeInit = 0x8be60;
constexpr uint64_t kOriginView = 0x8c280;
constexpr uint64_t kObjBaseOffset = 0x10; // obj+0x10: u64 base, written by the creator
constexpr uint64_t kObjTypeOffset = 0x1c; // obj+0x1c: u32 type
constexpr uint64_t kObjSizeOffset = 0x20; // obj+0x20: u32 size bytes

struct Pair {
    uint64_t hash = 0;
    uint32_t binding = 0;
    // Window (reset by Report).
    uint64_t w_binds = 0;
    uint64_t w_dirty = 0;
    // Last bind.
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t vsize = 0;
    uint32_t arena = 0;
    // First bind: logged once.
    bool logged = false;
};

struct ObjEntry {
    uint64_t obj = 0;
    uint64_t size = 0;
    uint32_t type = 0;
    uint32_t origin = 0;
    bool used = false;
};

struct ObjInfo {
    uint64_t obj = 0;
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t type = 0;
    uint32_t origin = 0;
};

struct Caller {
    uint64_t va = 0; // game VA of the return address
    uint64_t w_count = 0;
    uint64_t total = 0;
    uint32_t type = 0;
    uint32_t stride = 0;
    uint32_t flags = 0;
};

std::mutex g_mutex; // pairs
std::vector<Pair> g_pairs;
uint64_t g_w_binds = 0; // unbounded binds of the window, all pairs (overflow pairs included)
thread_local uint64_t t_dirty = 0;

std::mutex g_obj_mutex; // object ring and -1 callers
std::vector<ObjEntry> g_objs;
size_t g_obj_next = 0;
std::vector<Caller> g_callers;

const SotcSrtSource* FindSource(const Shader::Info& info, uint32_t dst) {
    for (const SotcSrtSource& s : info.sotc_srt_sources) {
        if (s.dst_off_dw == dst) {
            return &s;
        }
    }
    return nullptr;
}

// The table that holds the entry: the root pointer from the SGPR pair, then each pointer load of the path.
bool ResolveTable(const Shader::Info& info, const SotcSrtSource& src, uint64_t& table) {
    const auto ud = info.UserData();
    if (!src.valid || src.path.empty() || src.path[0] + 1 >= ud.size()) {
        return false;
    }
    uint64_t p = (uint64_t{ud[src.path[0]]} | (uint64_t{ud[src.path[0] + 1]} << 32)) & kMask48;
    for (size_t i = 1; i < src.path.size(); ++i) {
        uint64_t next = 0;
        if (!SotcGuest::Read(p + uint64_t{src.path[i]} * 4, &next, sizeof next)) {
            return false;
        }
        p = next & kMask48;
    }
    table = p;
    return p != 0;
}

// Text for where the V# words are read, and the V# address when it is in guest memory.
std::string DescribeSharp(const Shader::Info& info, uint32_t k, bool& has_vaddr, uint64_t& vaddr) {
    char buf[256];
    has_vaddr = false;
    if (k < kUserDataDwords) {
        std::snprintf(buf, sizeof buf,
                      "user data s%u..s%u (V# in SGPRs, set by PM4; no guest address)", k, k + 3);
        return buf;
    }
    const SotcSrtSource* src = FindSource(info, k);
    if (!src) {
        return "SRT, no provenance (Info from the pipeline cache, or flattened before the probe)";
    }
    uint64_t table = 0;
    if (!ResolveTable(info, *src, table)) {
        std::snprintf(buf, sizeof buf, "SRT, path not resolvable (root s%u%s)",
                      src->path.empty() ? 0u : src->path[0], src->valid ? ", unreadable" : ", dynamic");
        return buf;
    }
    has_vaddr = true;
    vaddr = table + uint64_t{src->src_off_dw} * 4;
    std::snprintf(buf, sizeof buf, "SRT table %#llx + %#x (root s%u, %zu pointer load(s))",
                  static_cast<unsigned long long>(table), src->src_off_dw * 4u, src->path[0],
                  src->path.size() - 1);
    return buf;
}

void AppendDwords(std::string& line, const uint8_t* p, size_t bytes) {
    char tmp[16];
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t v = 0;
        std::memcpy(&v, p + i, sizeof v);
        std::snprintf(tmp, sizeof tmp, " %08x", v);
        line += tmp;
    }
}

// Records one buffer object's interval. Caller holds nothing; takes g_obj_mutex.
void NoteObj(uint64_t obj, uint64_t size, uint32_t type, uint64_t origin) {
    if (obj == 0 || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_obj_mutex);
    if (g_objs.empty()) {
        g_objs.resize(kObjSlots);
    }
    g_objs[g_obj_next % kObjSlots] = ObjEntry{obj, size, type, static_cast<uint32_t>(origin), true};
    ++g_obj_next;
}

// The newest object-map entry whose base (read from obj+0x10 now) and size cover addr. Takes g_obj_mutex;
// the caller must not hold it. Up to 65536 guest reads when nothing matches: used once per pair and per report.
bool FindObject(uint64_t addr, ObjInfo* out) {
    std::lock_guard<std::mutex> lock(g_obj_mutex);
    const size_t n = g_objs.size();
    for (size_t i = 0; i < n; ++i) {
        const ObjEntry& e = g_objs[(g_obj_next + n - 1 - i) % n];
        if (!e.used) {
            continue;
        }
        uint64_t base = 0;
        if (!SotcGuest::Read(e.obj + kObjBaseOffset, &base, sizeof base) || base == 0) {
            continue;
        }
        if (addr >= base && addr - base < e.size) {
            *out = ObjInfo{e.obj, base, e.size, e.type, e.origin};
            return true;
        }
    }
    return false;
}

// One line per pair, the first time it is seen: where the V# is, the block that holds the base, the object
// map answer, and the neighbourhood dump (the 16 V# bytes with 64 bytes either side, or all user data dwords).
void LogOnce(const Shader::Info& info, uint32_t binding, uint32_t k, uint64_t vbase, uint32_t vsize,
             uint64_t size, uint64_t dirty, const AmdGpu::Buffer& s) {
    bool has_vaddr = false;
    uint64_t vaddr = 0;
    const std::string loc = DescribeSharp(info, k, has_vaddr, vaddr);
    SotcProbes::BlockInfo blk{};
    const bool have_blk = SotcProbes::FindBlock(vbase, &blk);
    const int ring = SotcFrameRings::RingOf(vbase, size);
    char where[128];
    if (has_vaddr) {
        std::snprintf(where, sizeof where, "V# at %#llx", static_cast<unsigned long long>(vaddr));
    } else {
        std::snprintf(where, sizeof where, "V# not in guest memory");
    }
    char blkbuf[192];
    if (have_blk) {
        std::snprintf(blkbuf, sizeof blkbuf,
                      "block %#llx+%#llx (kind %d, %s, base at +%#llx, %#llx to block end)",
                      static_cast<unsigned long long>(blk.base), static_cast<unsigned long long>(blk.size),
                      blk.kind, blk.pool, static_cast<unsigned long long>(vbase - blk.base),
                      static_cast<unsigned long long>(blk.base + blk.size - vbase));
    } else {
        std::snprintf(blkbuf, sizeof blkbuf, "block unknown (SOTC_HOOK_ALLOC_PROBE off or base untracked)");
    }
    const char* ring_name = ring < 0 ? "unknown" : ring == 0 ? "none" : ring == 1 ? "cpu" : "gpu";
    std::fprintf(stderr,
                 "SOTC VSIZE: %#llx/b%u sharp at %s, %s, base %#llx, %s, ring %s, arena %d, "
                 "num_records %#x stride %u vsize %#x, first bind size %#llx dirty %.1f MiB, "
                 "writer not measured\n",
                 static_cast<unsigned long long>(info.pgm_hash), binding, loc.c_str(), where,
                 static_cast<unsigned long long>(vbase), blkbuf, ring_name,
                 SotcPcRing::ArenaRelation(vbase, size), s.num_records, static_cast<unsigned>(s.stride),
                 vsize, static_cast<unsigned long long>(size), dirty / 1048576.0);

    ObjInfo oi{};
    if (FindObject(vbase, &oi)) {
        std::fprintf(stderr,
                     "SOTC VSIZE OBJ: %#llx/b%u base %#llx in object %#llx (origin %#llx, obj base %#llx, "
                     "size %#llx, type %u): remaining %#llx\n",
                     static_cast<unsigned long long>(info.pgm_hash), binding,
                     static_cast<unsigned long long>(vbase), static_cast<unsigned long long>(oi.obj),
                     static_cast<unsigned long long>(oi.origin), static_cast<unsigned long long>(oi.base),
                     static_cast<unsigned long long>(oi.size), oi.type,
                     static_cast<unsigned long long>(oi.size - (vbase - oi.base)));
    } else {
        std::fprintf(stderr, "SOTC VSIZE OBJ: %#llx/b%u base %#llx: no object-map entry covers it\n",
                     static_cast<unsigned long long>(info.pgm_hash), binding,
                     static_cast<unsigned long long>(vbase));
    }

    // Neighbourhood dump.
    std::string dump;
    dump.reserve(1024);
    if (has_vaddr) {
        uint8_t buf[kBefore + 16 + kAfter];
        if (SotcGuest::Read(vaddr - kBefore, buf, sizeof buf)) {
            dump += "SOTC VSIZE DUMP: before-64:";
            AppendDwords(dump, buf, kBefore);
            dump += " | V#:";
            AppendDwords(dump, buf + kBefore, 16);
            dump += " | after-64:";
            AppendDwords(dump, buf + kBefore + 16, kAfter);
        } else {
            dump = "SOTC VSIZE DUMP: V# neighbourhood unreadable (outside a mapped region)";
        }
    } else {
        dump = "SOTC VSIZE DUMP: user data dwords:";
        const auto ud = info.UserData();
        for (uint32_t i = 0; i < ud.size(); ++i) {
            char tmp[24];
            std::snprintf(tmp, sizeof tmp, " s%u=%08x", i, ud[i]);
            dump += tmp;
        }
    }
    std::fprintf(stderr, "%s\n", dump.c_str());
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_VSIZE_PROBE");
        return v && v[0] == '1';
    }();
    return on;
}

void ResetDirty() noexcept {
    t_dirty = 0;
}

void NoteDirty(uint64_t bytes) noexcept {
    t_dirty += bytes;
}

uint64_t Dirty() noexcept {
    return t_dirty;
}

void NoteBind(const Shader::Info& info, const Shader::BufferResource& desc, uint32_t buf_index,
              uint64_t vbase, uint32_t vsize, uint64_t size, uint64_t dirty_bytes) {
    const AmdGpu::Buffer s = desc.GetSharp(info);
    const uint32_t k = desc.sharp_fetch.offsets[0];
    const uint64_t hash = info.pgm_hash;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_w_binds;
    Pair* p = nullptr;
    for (Pair& q : g_pairs) {
        if (q.hash == hash && q.binding == buf_index) {
            p = &q;
            break;
        }
    }
    if (!p) {
        if (g_pairs.size() >= kMaxPairs) {
            return; // the window count above still holds the bind
        }
        g_pairs.push_back(Pair{});
        p = &g_pairs.back();
        p->hash = hash;
        p->binding = buf_index;
    }
    p->w_binds += 1;
    p->w_dirty += dirty_bytes;
    p->base = vbase;
    p->size = size;
    p->vsize = vsize;
    p->arena = static_cast<uint32_t>(SotcPcRing::ArenaRelation(vbase, size));
    if (!p->logged) {
        p->logged = true;
        // sotc: where each V# dword comes from: loaded from the flat buffer (offset) or an immediate of the shader
        // (a descriptor the shader builds itself, e.g. num_records -1 from s_mov).
        const auto& f = desc.sharp_fetch;
        std::fprintf(stderr,
                     "SOTC VSIZE FETCH: %#llx/b%u summary %d load_mask %#x dw0 %s%#x dw1 %s%#x dw2 %s%#x dw3 %s%#x\n",
                     static_cast<unsigned long long>(hash), buf_index, static_cast<int>(f.summary), f.load_mask,
                     (f.load_mask & 1) ? "flat+" : "imm ", (f.load_mask & 1) ? f.offsets[0] : f.immediates[0],
                     (f.load_mask & 2) ? "flat+" : "imm ", (f.load_mask & 2) ? f.offsets[1] : f.immediates[1],
                     (f.load_mask & 4) ? "flat+" : "imm ", (f.load_mask & 4) ? f.offsets[2] : f.immediates[2],
                     (f.load_mask & 8) ? "flat+" : "imm ", (f.load_mask & 8) ? f.offsets[3] : f.immediates[3]);
        LogOnce(info, buf_index, k, vbase, vsize, size, dirty_bytes, s);
    }
}

// NATIVE_ZC_CREATE pre handler (entry of 0x8be60), before its own logic. Observe only.
void NoteBpeEntry(const GuestRegs& regs) {
    if (!On()) {
        return;
    }
    const uint32_t count = static_cast<uint32_t>(regs.rcx);
    const uint32_t stride = static_cast<uint32_t>(regs.rdx);
    const uint32_t type = static_cast<uint32_t>(regs.rsi);
    const uint32_t flags = static_cast<uint32_t>(regs.r8);
    if (count == kCountMinusOne) {
        uint64_t ret = 0;
        if (!SotcGuest::Read(regs.rsp, &ret, sizeof ret)) {
            ret = 0;
        }
        const uint64_t image = static_cast<uint64_t>(SotcHooks::ImageBase());
        const uint64_t va = (image != 0 && ret >= image) ? ret - image : ret;
        std::lock_guard<std::mutex> lock(g_obj_mutex);
        Caller* c = nullptr;
        for (Caller& q : g_callers) {
            if (q.va == va) {
                c = &q;
                break;
            }
        }
        if (!c && g_callers.size() < kMaxCallers) {
            g_callers.push_back(Caller{});
            c = &g_callers.back();
            c->va = va;
            std::fprintf(stderr,
                         "SOTC VSIZE: -1 count from caller %#llx (game VA): first seen, type %u stride %u "
                         "flags %#x\n",
                         static_cast<unsigned long long>(va), type, stride, flags);
        }
        if (c) {
            c->w_count += 1;
            c->total += 1;
            c->type = type;
            c->stride = stride;
            c->flags = flags;
        }
        return; // a -1 count gives no usable size: not in the object map
    }
    // The callee computes the same 32-bit product (imul of count and stride) for obj+0x20.
    NoteObj(regs.rdi, static_cast<uint32_t>(count * stride), type, kOriginBpeInit);
}

// VSIZE_VIEW_SITE handler: site 0x8c2e2 in 0x8c280, after the store of obj+0x18..0x20. Observe only.
HookAction ViewSite(GuestRegs& regs, void*) {
    if (!On()) {
        return HookAction::Continue;
    }
    uint32_t size = 0;
    uint32_t type = 0;
    if (SotcGuest::Read(regs.rdi + kObjSizeOffset, &size, sizeof size) &&
        SotcGuest::Read(regs.rdi + kObjTypeOffset, &type, sizeof type)) {
        NoteObj(regs.rdi, size, type, kOriginView);
    }
    return HookAction::Continue;
}

void RegisterVsizeHooks() {
    SotcHooks::RegisterHandler("VSIZE_VIEW_SITE", SotcHooks::Handler{ViewSite, nullptr, nullptr});
}

void Report(std::string& out, double seconds) {
    if (!On() || seconds <= 0.0) {
        return;
    }
    struct Row {
        uint64_t hash;
        uint32_t binding;
        uint64_t binds;
        uint64_t dirty;
        uint64_t base;
        uint64_t size;
        uint32_t arena;
    };
    std::vector<Row> rows;
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        total = g_w_binds;
        g_w_binds = 0;
        for (Pair& p : g_pairs) {
            if (p.w_binds) {
                rows.push_back(Row{p.hash, p.binding, p.w_binds, p.w_dirty, p.base, p.size, p.arena});
            }
            p.w_binds = 0;
            p.w_dirty = 0;
        }
    }
    // -1 counts per caller: the top ones of the window.
    std::vector<Caller> callers;
    {
        std::lock_guard<std::mutex> lock(g_obj_mutex);
        callers = g_callers;
        for (Caller& c : g_callers) {
            c.w_count = 0;
        }
    }
    if (total == 0) {
        return;
    }
    char head[128];
    std::snprintf(head, sizeof head, "SOTC VSIZE PROBE: unbounded binds %.1f/s, by pair:",
                  static_cast<double>(total) / seconds);
    out += head;
    for (const Row& r : rows) {
        SotcProbes::BlockInfo blk{};
        const bool have_blk = SotcProbes::FindBlock(r.base, &blk);
        const int ring = SotcFrameRings::RingOf(r.base, r.size);
        const char* ring_name = ring < 0 ? "unknown" : ring == 0 ? "none" : ring == 1 ? "cpu" : "gpu";
        ObjInfo oi{};
        const bool have_obj = FindObject(r.base, &oi);
        char line[512];
        const unsigned long long hash = static_cast<unsigned long long>(r.hash);
        const unsigned long long obj_remaining =
            have_obj ? static_cast<unsigned long long>(oi.size - (r.base - oi.base)) : 0ULL;
        if (have_blk) {
            std::snprintf(line, sizeof line,
                          " %#llx/b%u base-in-block %#llx/%#llx to-end %#llx ring %s arena %d dirty %.1f MiB/%.1f s"
                          "%s;",
                          hash, r.binding, static_cast<unsigned long long>(r.base - blk.base),
                          static_cast<unsigned long long>(blk.size),
                          static_cast<unsigned long long>(blk.base + blk.size - r.base), ring_name, r.arena,
                          r.dirty / 1048576.0, seconds,
                          have_obj ? (std::string(" obj remaining ") + std::to_string(obj_remaining)).c_str()
                                   : " obj none");
        } else {
            std::snprintf(line, sizeof line,
                          " %#llx/b%u base-in-block ?/? ring %s arena %d dirty %.1f MiB/%.1f s%s;", hash,
                          r.binding, ring_name, r.arena, r.dirty / 1048576.0, seconds,
                          have_obj ? " obj present" : " obj none");
        }
        out += line;
    }
    out += "\n";
    if (!callers.empty()) {
        std::string line = "SOTC VSIZE: -1 count callers this window:";
        size_t shown = 0;
        for (const Caller& c : callers) {
            if (c.w_count == 0 || shown >= 8) {
                continue;
            }
            char tmp[160];
            std::snprintf(tmp, sizeof tmp, " caller %#llx n %llu (type %u stride %u flags %#x);",
                          static_cast<unsigned long long>(c.va), static_cast<unsigned long long>(c.w_count),
                          c.type, c.stride, c.flags);
            line += tmp;
            ++shown;
        }
        if (shown) {
            out += line + "\n";
        }
    }
}

} // namespace SotcVsizeProbe
