// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_STATIC_CLASS (static_class.h; docs/re-stutters/PC-MEMORY-REWRITE.md, reports3/03).
// The GPU_STATIC class of the allocations a resource build makes for its GPU copies:
//   mesh build (0x1264d0) and texture build (0x98aa0) set a per-thread build depth;
//   the buffer wrapper 0x8be60 (GPUMemory, flags bits 0x800/0x2/0x18 clear) and the single-block
//   texture copy 0x96680 (after its 0x12820 call) hand the fresh range to the tracker, before the
//   game's copy fills it.
// SOTC_STATIC_CLASS=2 counts only (ClassifyFreshRange with apply = false). =1 applies: the whole
// pages of the range whose tracking region exists and that have no GPU bit become CPU-clean and
// write-protected (MemoryTracker::ClassifyFreshRange), so the fill faults page by page.
// The handlers run on the guest thread (host stack), allocate nothing and log nothing per call.
//
// Mode 3 (apply + skip): also registers each classified mesh allocation (its base, size and
// "armed" state). The buffer cache's bind of a mesh range that was armed by a clean full sync
// skips the page scan (ForEachUploadRange) while the range's sync generation sum is unchanged.
// Why this is the same as the scan: a clean scan means no CPU bit in the range; from then on a
// CPU bit can only rise through UpdateStateAndProtection, which bumps the granule generation
// after the store (region_manager.h, SOTC_SYNC_FAST stamps). Generations only grow, so an equal
// sum means no bump, hence no new CPU bit. Protection: a page without a CPU bit is write-
// protected (protected = cpu == 0 with the ring off), so a guest write to it faults, sets the bit
// and bumps the generation before the page is unprotected. A read bind (not is_written) with a
// clean range needs no upload and no GPU bit, so the skip is exact for it; written bindings always
// take the full path. Arming records the gen0 that the caller loaded before its scan, so any
// bump between gen0 and the scan only makes the record conservative.
// Rewrites (streaming reuse, the game's own refills) fault and bump: the skip stops at the next
// bind (counted as a rewrite, the range is disarmed) and the full path re-arms it when it finds the
// range clean again. A new classify of the same address erases the old entries (the allocation was
// freed and reused).

#include "static_class.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

#include "hooks.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_manager.h"

namespace SotcStaticClass {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

// Build depth of the resource factories on this thread (set by the build hooks, nested-safe).
thread_local int t_mesh_depth = 0;
thread_local int t_tex_depth = 0;

// SOTC_STATIC_CLASS value: 0 off, 1 apply, 2 count only, 3 apply + skip. Read from the hook once
// the hooks are installed (handlers run only then); -1 until read.
int Mode() {
    static std::atomic<int> cached{-1};
    int mode = cached.load(std::memory_order_relaxed);
    if (mode < 0) {
        mode = SotcHooks::HookMode("STATIC_CLASS_BUFFER_ALLOC");
        cached.store(mode, std::memory_order_relaxed);
    }
    return mode;
}

// The mode that runs: 3 needs the sync generation stamps (region_manager.h); without them it is 1.
int EffectiveMode() {
    const int mode = Mode();
    if (mode == 3 && !VideoCore::SotcBufBindFast::kGenStamps) {
        return 1;
    }
    return mode;
}

// Mode 3 registry. Keyed by the allocation base; entries never overlap (a new allocation erases
// the entries it overlaps). Guarded by g_registry_mutex: the guest thread (classify) and the draw
// thread (TrySkip / NoteClean) both use it, neither holds it across a tracker call.
struct RegistryEntry {
    uint64_t size = 0;      // the classified allocation [key, key + size)
    bool armed = false;     // a clean full sync recorded armed_addr/armed_size at gen
    uint64_t armed_addr = 0;
    uint64_t armed_size = 0;
    uint64_t gen = 0;
};
std::mutex g_registry_mutex;
std::map<uint64_t, RegistryEntry> g_registry;

// Registry counters (window; reset by Report). Written under g_registry_mutex.
uint64_t g_reg_inserted = 0;   // classified mesh allocations with clean pages
uint64_t g_reg_forgotten = 0;  // entries erased by an overlapping allocation
uint64_t g_reg_armed = 0;      // NoteClean arms
uint64_t g_reg_skips = 0;      // binds that skipped the scan
uint64_t g_reg_skip_bytes = 0;
uint64_t g_reg_rewrites = 0;   // armed ranges whose sum changed (disarmed)
uint64_t g_reg_unarmed = 0;    // binds inside a static allocation that were not armed

// Erases every entry overlapping [addr, addr + size). Caller holds g_registry_mutex.
void EraseOverlapping(uint64_t addr, uint64_t size) {
    auto it = g_registry.lower_bound(addr);
    if (it != g_registry.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second.size > addr) {
            it = prev;
        }
    }
    while (it != g_registry.end() && it->first < addr + size) {
        ++g_reg_forgotten;
        it = g_registry.erase(it);
    }
}

// The entry whose allocation contains [addr, addr + size), or null. Caller holds the mutex.
RegistryEntry* Containing(uint64_t addr, uint64_t size) {
    auto it = g_registry.upper_bound(addr);
    if (it == g_registry.begin()) {
        return nullptr;
    }
    --it; // the last entry with base <= addr
    const uint64_t offset = addr - it->first;
    if (offset > it->second.size || size > it->second.size - offset) {
        return nullptr;
    }
    return &it->second;
}

// Classify, when the mode is 3: drops the entries of the range and registers it when any of its
// pages was made clean. Registration is only a hint; the arm needs a clean full sync (NoteClean).
void RegistryFresh(uint64_t addr, uint64_t size, bool clean_pages) {
    std::scoped_lock lock{g_registry_mutex};
    EraseOverlapping(addr, size);
    if (clean_pages && g_registry.size() < 65536) { // bounded: a full map just stays on the scan
        g_registry[addr] = RegistryEntry{size, false, 0, 0, 0};
        ++g_reg_inserted;
    }
}

// Window counters of one build kind. Lock-free; Report takes and resets them.
struct Window {
    std::atomic<uint64_t> ranges{0};     // fresh allocations seen during a build
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> found{0};      // ranges with at least one page in an existing region
    std::atomic<uint64_t> pages{0};      // whole pages of those ranges
    std::atomic<uint64_t> tracked{0};    // of those, pages in an existing region
    std::atomic<uint64_t> gpu{0};        // tracked pages with a GPU bit (left alone)
    std::atomic<uint64_t> clean{0};      // pages made CPU-clean (applied, or would be in mode 2)
    std::atomic<uint64_t> missing{0};    // 16 MiB spans without a tracking region
    std::atomic<uint64_t> no_tracker{0}; // ranges seen while no tracker was live
};
Window g_mesh;
Window g_tex;

void Classify(Window& w, uint64_t addr, uint64_t size) {
    if (addr == 0 || size == 0) {
        return;
    }
    w.ranges.fetch_add(1, std::memory_order_relaxed);
    w.bytes.fetch_add(size, std::memory_order_relaxed);
    const int mode = EffectiveMode();
    // Mode 3 registers mesh allocations only (texture binds never reach SynchronizeMemory). The
    // overlapping entries go first, whatever the tracker state: the range is a new allocation.
    const bool registry = mode == 3 && &w == &g_mesh;
    VideoCore::MemoryTracker* tracker = VideoCore::MemoryTracker::Current();
    if (!tracker) {
        if (registry) {
            RegistryFresh(addr, size, false);
        }
        w.no_tracker.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto st = tracker->ClassifyFreshRange(addr, size, mode == 1 || mode == 3);
    if (registry) {
        RegistryFresh(addr, size, st.pages_clean != 0);
    }
    if (st.pages_tracked != 0) {
        w.found.fetch_add(1, std::memory_order_relaxed);
    }
    w.pages.fetch_add(st.pages, std::memory_order_relaxed);
    w.tracked.fetch_add(st.pages_tracked, std::memory_order_relaxed);
    w.gpu.fetch_add(st.pages_gpu, std::memory_order_relaxed);
    w.clean.fetch_add(st.pages_clean, std::memory_order_relaxed);
    w.missing.fetch_add(st.spans_missing, std::memory_order_relaxed);
}

// Pre phase of STATIC_CLASS_BUFFER_ALLOC, site at 0x8bf0b (before mov r15,rax). rax = the pointer
// returned by the 0x12820 call, rbx = the buffer object (size u32 at +0x20, written before the
// allocation), r12 = flags & 1 (0 selects GPUMemory). r15 is not the pointer yet at this site.
HookAction BufferAllocPre(GuestRegs& regs, void*) {
    if (regs.r12 != 0) {
        return HookAction::Continue; // CPUMemory allocation
    }
    Window* w = t_mesh_depth > 0 ? &g_mesh : (t_tex_depth > 0 ? &g_tex : nullptr);
    if (!w) {
        return HookAction::Continue; // not a resource build: the wrapper has many other callers
    }
    uint32_t size = 0;
    std::memcpy(&size, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rbx + 0x20)),
                sizeof size);
    Classify(*w, regs.rax, size);
    return HookAction::Continue;
}

// Pre phase of STATIC_CLASS_TEX_ALLOC, site at 0x96b48 (the single-block path). rax = the pointer,
// rbx = the size. 0x96680 is also called by the particle build, which is not hooked, so the
// texture build depth is what gates it.
HookAction TexAllocPre(GuestRegs& regs, void*) {
    if (t_tex_depth > 0) {
        Classify(g_tex, regs.rax, regs.rbx);
    }
    return HookAction::Continue;
}

HookAction MeshBuildPre(GuestRegs&, void*) {
    ++t_mesh_depth;
    return HookAction::Continue;
}
void MeshBuildPost(GuestRegs&, void*) {
    if (t_mesh_depth > 0) {
        --t_mesh_depth;
    }
}

HookAction TexBuildPre(GuestRegs&, void*) {
    ++t_tex_depth;
    return HookAction::Continue;
}
void TexBuildPost(GuestRegs&, void*) {
    if (t_tex_depth > 0) {
        --t_tex_depth;
    }
}

struct Snapshot {
    uint64_t ranges, bytes, found, pages, tracked, gpu, clean, missing, no_tracker;
};

Snapshot Take(Window& w) {
    return Snapshot{
        w.ranges.exchange(0, std::memory_order_relaxed),
        w.bytes.exchange(0, std::memory_order_relaxed),
        w.found.exchange(0, std::memory_order_relaxed),
        w.pages.exchange(0, std::memory_order_relaxed),
        w.tracked.exchange(0, std::memory_order_relaxed),
        w.gpu.exchange(0, std::memory_order_relaxed),
        w.clean.exchange(0, std::memory_order_relaxed),
        w.missing.exchange(0, std::memory_order_relaxed),
        w.no_tracker.exchange(0, std::memory_order_relaxed),
    };
}

void AppendKind(char* buf, size_t cap, const char* label, const Snapshot& s) {
    std::snprintf(buf, cap,
                  "%s: ranges=%llu MiB=%.1f found=%llu pages=%llu tracked=%llu gpu=%llu "
                  "clean=%llu missing_spans=%llu no_tracker=%llu",
                  label, static_cast<unsigned long long>(s.ranges),
                  static_cast<double>(s.bytes) / (1024.0 * 1024.0),
                  static_cast<unsigned long long>(s.found),
                  static_cast<unsigned long long>(s.pages),
                  static_cast<unsigned long long>(s.tracked),
                  static_cast<unsigned long long>(s.gpu),
                  static_cast<unsigned long long>(s.clean),
                  static_cast<unsigned long long>(s.missing),
                  static_cast<unsigned long long>(s.no_tracker));
}

} // namespace

void Register() {
    SotcHooks::Handler mesh_build;
    mesh_build.pre = &MeshBuildPre;
    mesh_build.post = &MeshBuildPost;
    SotcHooks::RegisterHandler("STATIC_CLASS_MESH_BUILD", mesh_build);

    SotcHooks::Handler tex_build;
    tex_build.pre = &TexBuildPre;
    tex_build.post = &TexBuildPost;
    SotcHooks::RegisterHandler("STATIC_CLASS_TEX_BUILD", tex_build);

    SotcHooks::Handler buffer_alloc;
    buffer_alloc.pre = &BufferAllocPre;
    SotcHooks::RegisterHandler("STATIC_CLASS_BUFFER_ALLOC", buffer_alloc);

    SotcHooks::Handler tex_alloc;
    tex_alloc.pre = &TexAllocPre;
    SotcHooks::RegisterHandler("STATIC_CLASS_TEX_ALLOC", tex_alloc);
}

bool Skip3Enabled() {
    return EffectiveMode() == 3;
}

bool TrySkip(uint64_t addr, uint64_t size, uint64_t gen0) {
    if (size == 0 || !Skip3Enabled()) {
        return false;
    }
    std::scoped_lock lock{g_registry_mutex};
    RegistryEntry* e = Containing(addr, size);
    if (!e) {
        return false; // not a static allocation: the normal path, not counted
    }
    if (!e->armed || e->armed_addr != addr || e->armed_size != size) {
        ++g_reg_unarmed;
        return false;
    }
    if (e->gen != gen0) {
        // A bump since the arm: a CPU bit rose in the range (a rewrite). Disarm; the full path
        // re-arms it once a scan finds the range clean again.
        e->armed = false;
        ++g_reg_rewrites;
        return false;
    }
    ++g_reg_skips;
    g_reg_skip_bytes += size;
    return true;
}

void NoteClean(uint64_t addr, uint64_t size, uint64_t gen0) {
    if (size == 0 || !Skip3Enabled()) {
        return;
    }
    std::scoped_lock lock{g_registry_mutex};
    RegistryEntry* e = Containing(addr, size);
    if (!e) {
        return;
    }
    e->armed = true;
    e->armed_addr = addr;
    e->armed_size = size;
    e->gen = gen0;
    ++g_reg_armed;
}

void Report(std::string& out, double seconds) {
    const int mode = Mode();
    if (mode == 0) {
        return;
    }
    const int eff = EffectiveMode();
    const Snapshot mesh = Take(g_mesh);
    const Snapshot tex = Take(g_tex);
    char mesh_line[384];
    char tex_line[384];
    AppendKind(mesh_line, sizeof mesh_line, "mesh", mesh);
    AppendKind(tex_line, sizeof tex_line, "texture", tex);
    char line[1024];
    std::snprintf(line, sizeof line, "SOTC STATIC CLASS: mode=%d (%s) window=%.1fs %s; %s\n", mode,
                  eff == 1 ? "apply" : (eff == 3 ? "apply+skip" : "count"), seconds, mesh_line,
                  tex_line);
    out += line;
    if (mode != 3) {
        return;
    }
    // Registry state, then the window counters (reset here, under the same mutex as the users).
    std::scoped_lock lock{g_registry_mutex};
    size_t armed = 0;
    for (const auto& [base, e] : g_registry) {
        armed += e.armed ? 1 : 0;
    }
    char skip[512];
    std::snprintf(
        skip, sizeof skip,
        "SOTC STATIC SKIP: mode 3%s, %zu static allocations (%zu armed), over %.1f s: "
        "registered %llu, forgotten %llu, armed %llu, skipped binds %llu (%llu MiB), rewrites "
        "%llu, unarmed binds %llu\n",
        eff == 3 ? "" : " (no SOTC_SYNC_FAST / SOTC_BUF_BIND_FAST stamps: ran as 1)",
        g_registry.size(), armed, seconds, static_cast<unsigned long long>(g_reg_inserted),
        static_cast<unsigned long long>(g_reg_forgotten),
        static_cast<unsigned long long>(g_reg_armed),
        static_cast<unsigned long long>(g_reg_skips),
        static_cast<unsigned long long>(g_reg_skip_bytes >> 20),
        static_cast<unsigned long long>(g_reg_rewrites),
        static_cast<unsigned long long>(g_reg_unarmed));
    out += skip;
    g_reg_inserted = g_reg_forgotten = g_reg_armed = g_reg_skips = g_reg_skip_bytes = 0;
    g_reg_rewrites = g_reg_unarmed = 0;
}

} // namespace SotcStaticClass
