// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native write announces (docs/re-stutters/native/PLAN.md N2, declarations in
// scripts/hooks.d/native_writes.py, switch SOTC_NATIVE). Every hook sits after the engine finished a write (or
// before a release): Entry hooks on submits and creators read the arguments the writer already filled, Wrap
// hooks announce in the post phase, Site hooks sit after the store. Handlers only read registers and the guest
// words they name, then call SotcNative::Announce / Release / NoteDescriptor (native.h). Every handler returns
// Continue: the displaced instructions or the function body run next, unchanged.

#include "../preupload.h" // sotc: SOTC_PRELOAD_UPLOAD
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../../bbgpu.h"
#include "../hooks.h"
#include "native.h"
#include "../probe_vsize.h"  // sotc: SOTC_VSIZE_PROBE object map feed (NoteBpeEntry, observe only)
#include "video_core/sotc_native_ring.h" // sotc: the ACE ring announce (SotcNativeRing::Announce, below)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (null when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace SotcNativeHooks {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kMaxRange = 64ull << 20; // a single announced range above this is treated as a bad read

// Reads a guest word; 0 when the address is unmapped (the announce is then skipped).
template <typename T>
T GuestRead(uint64_t address) {
    const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(address));
    T value{};
    if (p) {
        std::memcpy(&value, p, sizeof(value));
    }
    return value;
}
uint64_t GuestU64(uint64_t address) {
    return GuestRead<uint64_t>(address);
}
uint32_t GuestU32(uint64_t address) {
    return GuestRead<uint32_t>(address);
}

// Caller filters: image offsets (EU 1.01) of the functions whose calls the GPU reads. A call from any other
// function is not announced (the 0x8c0d0 and 0x8be60 callers include many CPU-only users).
struct Span {
    uint32_t begin, end;
};

// Zero-copy (flag 0x800) creators: the per-frame particle and BPXPR builders, the 0x94fb0 draw buffers and the
// 0x198bb0 writer (heaps.md 7.4).
// impl-writes5 item 2: ResolveSunShadows 0xafcc0 and its twin 0xb0100 (per frame: reached from 0xafca0 by the
// shadow pass; the 0x10-byte backing is written before the 0x804 creator call), and 0x1af960 (the GPU5 blocks of
// the two 0x804 creators are filled by its loops before the creators run).
constexpr Span kZeroCopyCallers[] = {
    {0xffe60, 0xfff9e},   {0x19b760, 0x19c193}, {0x19c1a0, 0x19c732}, {0x19c7e0, 0x19ce60},
    {0x94fb0, 0x95313},   {0x198bb0, 0x19a1c4},
    {0xafcc0, 0xb0100},   {0xb0100, 0xb056e},   {0x1af960, 0x1afc33},
    // impl-writes6 item 5: 0xfedc0 creates the same two 0x804 buffers as the per-frame builder 0xffe60 (obj+0x28:
    // 0x70 per element, obj+0x50: 4 x 9 per element). No static caller (indirect); live status unknown. The span is
    // a superset (a PerFrame announce is harmless when the buffers are written once); the zc class shows if it runs.
    {0xfedc0, 0xfee84},
};

// The per-frame chunk writers of 0x8c0d0 (heaps.md 7.5: the 27 callers that memcpy into a chunk), the instance
// writers 0x11c710 and 0x123410, and EndFrameView_WriteA 0x8b7f0 (struct stores into a chunk).
constexpr Span kChunkCallers[] = {
    {0xb6e20, 0xb8c58},   {0x1371e0, 0x13758b}, {0xdd7c0, 0xddadd},   {0xddc90, 0xddf77},
    {0xdf300, 0xdf624},   {0xdf7e0, 0xdfad4},   {0x109ca0, 0x10a4fd}, {0x133fb0, 0x1342cb},
    {0x17c6c0, 0x17caba}, {0x186df0, 0x187196}, {0x18d6f0, 0x18e10d}, {0x18e3d0, 0x18e920},
    {0x18ed40, 0x18f823}, {0x18f8a0, 0x18fc6b}, {0x18fc70, 0x1905a9}, {0x190bc0, 0x19168a},
    {0x191700, 0x191a88}, {0x191cb0, 0x19259b}, {0x194a00, 0x19500e}, {0x198bb0, 0x19a1c4},
    {0x1a3e50, 0x1a42de}, {0x1a4800, 0x1a4b6d}, {0x1a4b70, 0x1a4ffe}, {0x1a5490, 0x1a57c8},
    {0x1acb10, 0x1acdba}, {0x1acdc0, 0x1ad056}, {0xaaf80, 0xab309},   {0x11c710, 0x11cf37},
    {0x123410, 0x12380a}, {0x8b7f0, 0x8b8da}, // EndFrameView_WriteA (heaps.md 7.5 chunk user, per frame)
    // impl-writes5 item 1: chunk-out callers of 0x8c0d0 that the GPU reads (decomp evidence per function).
    // Tags: DIRECT = the chunk reaches a bind (0x85ecd0/0x85e5d0), PM4 (0x16edc0/0x16ee00/0x16ee80/0x16eb00/
    // 0x16ed40), V# (0x865410 family), ZC (0x8be60) or DMA (0x16eb90), directly or in a callee within two hops;
    // SLOT = the chunk is stored in an object slot that the same function (or the bind records of its family)
    // reads back into a bind/PM4 record; HANDLE = the 0x8c0d0 block is the storage of a resource that 0x8be60
    // creates in the same function. The 37 SLOT+0xb0 rows without a sink in their body are inferred from the
    // family (the slot +0xb0 is copied into the bind records of sub_afcc0 and 0x1ad940); see impl-writes5.md.
    {0xa1e10, 0xa20ab},   // sub_a1e10 SLOT(+0xb0) + bind
    {0xa75b0, 0xa789b},   // sub_a75b0 SLOT(+0xb0) + PM4
    {0xa78a0, 0xa7ead},   // sub_a78a0 DIRECT bind
    {0xa7eb0, 0xa83ee},   // sub_a7eb0 SLOT(+0xb0) + PM4/bind
    {0xa83f0, 0xa86f2},   // sub_a83f0 DIRECT bind
    {0xafcc0, 0xb00fe},   // sub_afcc0 (ResolveSunShadows) SLOT(+0xb0) + PM4/bind
    {0xb50c0, 0xb5b05},   // sub_b50c0 DIRECT bind + PM4
    {0xb9710, 0xba57c},   // sub_b9710 VIA 0x93960 -> V# encoder 0x85fc10
    {0xd83b0, 0xd8653},   // sub_d83b0 DIRECT bind
    // Foliage displacer (VirtualGeometryDisplacement::UpdateJob 0xd8330). The prep 0xd81d0 fills the cluster records
    // (up to 64 x 0x20 bytes) with plain stores into a chunk whose pointer sits at [struct+0x50]; the job copies that
    // pointer into its own block (0xd83b0 reads [param_1+0x50]), so the GPU reads the records through it. The second
    // job variant 0xd8660 is the same chunk user with the cull dispatch 0x92b50 (see kCullDispatchCallers).
    {0xd81d0, 0xd8322},   // sub_d81d0 chunk of the displacer cluster records (plain stores)
    {0xd8660, 0xd892d},   // sub_d8660 DIRECT dispatch, chunk writer
    {0xe60c0, 0xe6342},   // sub_e60c0 SLOT(+0xb0) + PM4/bind
    {0xe69e0, 0xe738f},   // sub_e69e0 SLOT(+0xb0) + PM4/bind
    {0xe7390, 0xe7b53},   // sub_e7390 SLOT(+0xb0) + PM4/bind
    {0xe7b60, 0xe80cd},   // sub_e7b60 SLOT(+0xb0) + PM4/bind
    {0x103ad0, 0x1051e4}, // ParticleRenderer::UpdateSimulation DIRECT bind, SLOT(+0xb8) + PM4
    {0x107540, 0x1097cb}, // ParticleRenderer::BeginFrame VIA 0xffe60 (particle ZC creator)
    {0x10bfd0, 0x10c136}, // sub_10bfd0 SLOT(+0xb0) + PM4/bind
    {0x10c140, 0x10c2dc}, // sub_10c140 SLOT(+0xb0) + PM4/bind
    {0x10c8c0, 0x10cdad}, // sub_10c8c0 DIRECT PM4
    {0x10cf60, 0x10d1ac}, // sub_10cf60 DIRECT bind + PM4
    {0x10db10, 0x10df23}, // sub_10db10 SLOT(+0xb0) + PM4/bind
    {0x118470, 0x118ca0}, // Frame::InsertWaitFlipDone HANDLE (8be60 at 0x118470+0x220160)
    {0x17e6b0, 0x17e9b4}, // sub_17e6b0 SLOT(+0xb0) + PM4/bind
    {0x17ecf0, 0x17f788}, // sub_17ecf0 SLOT(+0xb0) + PM4
    {0x186540, 0x186a57}, // sub_186540 HANDLE (8be60 on local_80)
    {0x1871a0, 0x187506}, // sub_1871a0 HANDLE (8be60 on local_80)
    {0x187510, 0x187805}, // sub_187510 HANDLE (8be60 on local_80)
    {0x1893e0, 0x1899e9}, // sub_1893e0 SLOT(+0x98) + PM4
    {0x18a2c0, 0x18a448}, // sub_18a2c0 SLOT(+0xb0) + bind
    {0x18cf40, 0x18d3f8}, // sub_18cf40 HANDLE (8be60 on local_188)
    {0x1925a0, 0x1927c9}, // sub_1925a0 DIRECT bind
    {0x1a5000, 0x1a5301}, // sub_1a5000 DIRECT bind, SLOT(param_3+0xb0) + PM4
    {0x1ad940, 0x1adc83}, // sub_1ad940 DIRECT bind
    {0x1ade10, 0x1ae168}, // sub_1ade10 DIRECT bind
    {0x5d1540, 0x5d1746}, // sub_5d1540 HANDLE (8be60 on local_160)
    {0x5de0b0, 0x5de876}, // sub_5de0b0 DIRECT ZC (chunk is the 8be60 backing)
    {0x5deb40, 0x5df1e8}, // sub_5deb40 DIRECT ZC
    {0x5df1f0, 0x5df9a3}, // sub_5df1f0 DIRECT ZC + HANDLE
    {0x5df9b0, 0x5e056b}, // sub_5df9b0 HANDLE (8be60 on local_110)
    {0x5e09e0, 0x5e18b0}, // sub_5e09e0 DIRECT ZC + HANDLE
    {0x5e18b0, 0x5e2019}, // sub_5e18b0 DIRECT ZC
    {0x5e29f0, 0x5e3b49}, // sub_5e29f0 HANDLE (8be60 on param_3)
    {0x5e3b50, 0x5e410f}, // sub_5e3b50 HANDLE (8be60 on local_160)
    // SLOT(+0xb0) functions without a sink in their body (inferred from the family, see the comment above)
    {0xaf610, 0xafc99},   {0xb0100, 0xb056e},   {0xb0570, 0xb098a},   {0xb5b10, 0xb6588},
    {0xb6760, 0xb6e15},   {0xb8c60, 0xb9205},   {0xb9210, 0xb9608},   {0xba870, 0xbab3b},
    {0xbae70, 0xbb18d},   {0xbb310, 0xbb834},   {0xbbbf0, 0xbc00e},   {0xd79c0, 0xd7e95},
    {0xe6350, 0xe65f7},   {0xe84b0, 0xe8e7e},   {0xe8e80, 0xe9670},   {0xe9670, 0xe9c00},
    {0x1097d0, 0x109969}, {0x10c4e0, 0x10c6b3}, {0x10d890, 0x10db10}, {0x10df30, 0x10e38a},
    {0x1642d0, 0x16450e}, {0x17cac0, 0x17cd82}, {0x17e000, 0x17e5c2}, {0x17e9c0, 0x17ecf0},
    {0x17f790, 0x18028a}, {0x1802e0, 0x18056b}, {0x18a590, 0x18a719}, {0x1a28c0, 0x1a2bb6},
    {0x1a42e0, 0x1a4628}, {0x5d5820, 0x5d5e32},
    {0xa20b0, 0xa2368},   {0xa69d0, 0xa6de3},   {0xa8f80, 0xa9296},   {0xa92a0, 0xa9898},
    {0xa98a0, 0xa9e05},   {0xa9e10, 0xaa13e},   {0xaea10, 0xaf60b},
};

bool InSpans(uint64_t image_offset, const Span* spans, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (image_offset >= spans[i].begin && image_offset < spans[i].end) {
            return true;
        }
    }
    return false;
}

// The image offset of the caller of a function entered at a hook (Entry/Wrap: rsp points at the return
// address). Returns false when the caller lies outside the image.
bool CallerOffset(const GuestRegs& regs, uint64_t* offset) {
    const uint64_t ret = GuestU64(regs.rsp);
    const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
    if (ret == 0 || base == 0 || ret < base) {
        return false;
    }
    *offset = ret - base;
    return true;
}

// Per-window coverage counters (verify report: bytes announced per class per window).
struct Counter {
    std::atomic<uint64_t> n{0}, bytes{0};
    void Add(uint64_t size) {
        n.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(size, std::memory_order_relaxed);
    }
    uint64_t Take(std::atomic<uint64_t>& a) {
        return a.exchange(0, std::memory_order_relaxed);
    }
};

struct Window {
    std::atomic<uint64_t> res_announces{0}, res_bytes{0};
    std::atomic<uint64_t> res_gpu_bytes{0}, res_cpu_bytes{0};
    std::atomic<uint64_t> tex_announces{0}, tex_bytes{0};
    std::atomic<uint64_t> mip_announces{0}, mip_bytes{0};
    std::atomic<uint64_t> mip_releases{0}, mip_release_bytes{0};
    std::atomic<uint64_t> skipped{0};
    Counter dcb, ccb, chunk, zc, mat, blk, inst, part, view, strm, cull, rec, ring, ubuf;
    Counter sun, pkt, pal; // sotc: ResolveSunShadows blocks, user-data packets, cloth matrix palettes (n1verify misses)
    Counter gmem; // sotc: guest copies (runtime memcpy/memset/memmove) announced
    Counter file; // sotc: file reads (src/runtime_file.c) announced
    std::atomic<uint64_t> desc{0};
    std::atomic<uint64_t> chunk_dropped{0};
};
Window g_window;

void Count(std::atomic<uint64_t>& announces, std::atomic<uint64_t>& bytes, uint64_t size) {
    announces.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
}

// Announces [begin, end) when it is a sane range, counting it in c.
void AnnounceRange(uint64_t begin, uint64_t end, SotcNative::Kind kind, Counter& c) {
    if (begin == 0 || end <= begin) {
        return; // empty range: nothing was written (not a skip)
    }
    if (end - begin > kMaxRange) {
        g_window.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A pointer read from an object of another class (1 Nov: UTF-16 text 0x0053005000520052 from a 0x18-byte
    // instance block) is not a guest range: both ends must be mapped.
    if (!runtime_memory_backing_pointer(static_cast<uintptr_t>(begin)) ||
        !runtime_memory_backing_pointer(static_cast<uintptr_t>(end - 1))) {
        g_window.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    SotcNative::Announce(begin, end - begin, kind);
    c.Add(end - begin);
}

// ---------------------------------------------------------------------------------------------------------
// Write-after-announce guard (JITTER-BRIEF timing audit). A PerFrame announce is only a page mark: the bind that
// reads the page uploads the bytes it holds at that moment and clears the mark. Mode 0 had the write fault, which
// marked the page again for every CPU write after an upload. Mode 1 has nothing between the announce and the bind,
// so an announce made BEFORE the writer finished (a chunk flushed at a submit that is not the one that uses it, a
// block announced at the entry of its writer, a zero-copy backing announced at creation) lets any earlier bind of
// the same page (the draw thread lags the CPU; chunks are far smaller than a page; some binds cover whole heaps)
// consume the mark with the unfinished bytes, and the real bytes are never uploaded: the GPU draws the data of the
// frame that used the block before (stale camera and palette matrices, previous-frame matrices of the motion
// vectors one frame late). Where the exact copy of NativeDrain applies (range over GPU-written memory) the stale
// bytes are copied at drain time and no later bind fixes them.
//
// The guard keeps a hash of every small announced block for a few submits and announces it again when the bytes
// changed after the announce. A block written before its announce costs one hash per check; the announce
// protocol itself does not change. Counters: SotcNative::NoteLateWrite (Report "late writes").
constexpr uint32_t kTrackMaxSize = 64u << 10;   // larger blocks are not guarded
constexpr uint64_t kTrackMaxBytes = 768u << 10; // guarded bytes at a time; the oldest entries are dropped first
constexpr uint32_t kTrackMaxChecks = 16;        // submits an entry is checked for
constexpr uint64_t kTrackFrames = 2;            // ... and SubmitDone counts (frames)

struct Tracked {
    uint64_t addr;
    uint64_t hash;
    uint64_t frame; // SotcNative::FrameSubmitted() at the first announce
    uint32_t size;
    uint32_t checks;
    Counter* counter;
    bool expected; // announced at the entry of its writer: a change is the normal case, not counted as late
};
std::mutex g_track_mutex;
std::deque<Tracked> g_tracked;
uint64_t g_tracked_bytes = 0;

// Hash of the guest bytes [addr, addr + size). False when the range is not contiguous in the host view.
bool HashGuest(uint64_t addr, uint32_t size, uint64_t* out) {
    const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(addr));
    if (!p || size == 0 || runtime_memory_backing_pointer(static_cast<uintptr_t>(addr + size - 1)) != p + size - 1) {
        return false;
    }
    uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
    uint32_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, sizeof(w));
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < size; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    *out = h;
    return true;
}

// Announces [begin, end) PerFrame and guards it against a later write (see above).
void AnnounceTracked(uint64_t begin, uint64_t end, Counter& c, bool entry_hook = false) {
    uint64_t hash = 0;
    const bool track = end > begin && end - begin <= kTrackMaxSize && SotcNative::On() &&
                       HashGuest(begin, static_cast<uint32_t>(end - begin), &hash); // before the announce
    AnnounceRange(begin, end, SotcNative::Kind::PerFrame, c);
    if (!track) {
        return;
    }
    const uint32_t size = static_cast<uint32_t>(end - begin);
    const uint64_t frame = SotcNative::FrameSubmitted();
    std::lock_guard<std::mutex> lock(g_track_mutex);
    g_tracked.push_back(Tracked{begin, hash, frame, size, 0, &c, entry_hook});
    g_tracked_bytes += size;
    while (g_tracked_bytes > kTrackMaxBytes && !g_tracked.empty()) {
        g_tracked_bytes -= g_tracked.front().size;
        g_tracked.pop_front();
    }
}

// At every submit hook: announces again the guarded blocks whose bytes changed since their last announce, and
// retires the ones that were checked often enough or belong to an older frame.
void RecheckTracked() {
    if (!SotcNative::On()) {
        return;
    }
    struct Again {
        uint64_t addr;
        uint32_t size;
        bool same_frame;
        bool expected;
        Counter* counter;
    };
    std::vector<Again> again;
    const uint64_t frame = SotcNative::FrameSubmitted();
    {
        std::lock_guard<std::mutex> lock(g_track_mutex);
        size_t keep = 0;
        for (size_t i = 0; i < g_tracked.size(); ++i) {
            Tracked& t = g_tracked[i];
            uint64_t hash = 0;
            if (frame > t.frame + kTrackFrames || ++t.checks > kTrackMaxChecks || !HashGuest(t.addr, t.size, &hash)) {
                g_tracked_bytes -= t.size;
                continue;
            }
            if (hash != t.hash) {
                t.hash = hash;
                again.push_back(Again{t.addr, t.size, frame == t.frame, t.expected, t.counter});
            }
            if (keep != i) {
                g_tracked[keep] = t;
            }
            ++keep;
        }
        g_tracked.resize(keep);
    }
    for (const Again& a : again) {
        if (!a.expected) {
            SotcNative::NoteLateWrite(a.size, a.same_frame);
        }
        AnnounceRange(a.addr, a.addr + a.size, SotcNative::Kind::PerFrame, *a.counter);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Chunk list: blocks returned by 0x8c0d0 to the chunk writers, announced PerFrame at the next submit.
// The submit (0x91f80 / 0x16f100) is the one point where the GPU is about to read this frame's command
// buffers, and every chunk the writers filled before it is already complete (0xd07c0 waits on the jobs first).
struct ChunkBlock {
    uint64_t addr;
    uint32_t size;
};
std::mutex g_chunk_mutex;
std::vector<ChunkBlock> g_chunks;
constexpr size_t kChunkCap = 1u << 17;

void ChunkPush(uint64_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lock(g_chunk_mutex);
    if (g_chunks.size() >= kChunkCap) {
        g_window.chunk_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_chunks.push_back(ChunkBlock{addr, size});
}

void FlushChunks() {
    // sotc: blocks announced by an earlier flush (or at the entry of their writer) that changed since are
    // announced again before this flush (write-after-announce guard above).
    RecheckTracked();
    std::vector<ChunkBlock> batch;
    {
        std::lock_guard<std::mutex> lock(g_chunk_mutex);
        batch.swap(g_chunks);
    }
    for (const ChunkBlock& b : batch) {
        // A chunk allocated before this submit may still be written by a job of the next one: guarded.
        AnnounceTracked(b.addr, b.addr + b.size, g_window.chunk);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Submits (Entry). The DCB thunk 0x91f80 gets rdi = [context slot]; the 8636e0 submit reads the context:
// DCB base +0x00 and cursor +0x10, CCB base +0x40 and cursor +0x50, ACE base +0x80 and cursor +0x90.

// 0x91f80, before 0x8636e0 submits the DCB context: the context is at [rdi] (the thunk loads it).
HookAction DcbSubmit(GuestRegs& regs, void*) {
    if (!SotcNative::On()) {
        return HookAction::Continue;
    }
    const uint64_t ctx = GuestU64(regs.rdi);
    if (ctx != 0) {
        const uint64_t dcb_base = GuestU64(ctx + 0x00), dcb_cursor = GuestU64(ctx + 0x10);
        AnnounceRange(dcb_base, dcb_cursor, SotcNative::Kind::PerFrame, g_window.dcb);
        const uint64_t ccb_base = GuestU64(ctx + 0x40), ccb_cursor = GuestU64(ctx + 0x50);
        AnnounceRange(ccb_base, ccb_cursor, SotcNative::Kind::PerFrame, g_window.ccb);
        const uint64_t ace_base = GuestU64(ctx + 0x80), ace_cursor = GuestU64(ctx + 0x90);
        AnnounceRange(ace_base, ace_cursor, SotcNative::Kind::PerFrame, g_window.ccb);
    }
    FlushChunks();
    return HookAction::Continue;
}

// Site 0x91f97 in the flip submit thunk 0x91f90 (the frame's last DCB in a mode-1 job, KickoffSubmitJob 0xd07c0):
// the thunk did not run 0x91f80, so the same DCB context announce is needed here. rdi still holds the slot.
HookAction DcbSubmitFlip(GuestRegs& regs, void* user) {
    return DcbSubmit(regs, user);
}

// 0x16f100, before the CCB submit: the CCB object at rdi has 32-bit start (+0x10) and cursor (+0x20).
HookAction CcbSubmit(GuestRegs& regs, void*) {
    if (!SotcNative::On()) {
        return HookAction::Continue;
    }
    const uint64_t ccb = regs.rdi;
    if (ccb != 0) {
        const uint32_t start = GuestU32(ccb + 0x10), cursor = GuestU32(ccb + 0x20);
        if (cursor > start) {
            AnnounceRange(start, static_cast<uint64_t>(cursor & ~3u), SotcNative::Kind::PerFrame, g_window.ccb);
        }
    }
    FlushChunks();
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// Zero-copy creator 0x8be60 (Entry): (rdi = resource, esi = type, edx = elem, ecx = count, r8 = flags,
// r9 = backing). A zero-copy resource (0x800) uses the caller's backing as its storage; the caller filter
// keeps the per-frame builders in.
// The 0x8be60 call in progress on this thread (NATIVE_ZC_CREATE entry): the image offset of its return address and
// its second argument (the resource kind).
struct ResCreate {
    uint32_t caller;
    uint32_t kind;
};
thread_local ResCreate t_res_create{};

HookAction ZeroCopyCreate(GuestRegs& regs, void*) {
    SotcVsizeProbe::NoteBpeEntry(regs); // sotc: SOTC_VSIZE_PROBE (observe only; a no-op when the probe is off)
    if (SotcPreupload::On()) {
        // sotc: SOTC_PRELOAD_UPLOAD: who creates the resource whose initial-data memcpy follows (AnnounceWrite).
        uint64_t caller = 0;
        CallerOffset(regs, &caller);
        t_res_create = ResCreate{static_cast<uint32_t>(caller), static_cast<uint32_t>(regs.rsi)};
        // A buffer without initial data (no source, not zero-copy): a pool the CPU fills later.
        if (regs.r9 == 0 && (regs.r8 & 0x800) == 0 && static_cast<uint32_t>(regs.rcx) != 0xFFFFFFFFu) {
            const uint64_t size = uint64_t{static_cast<uint32_t>(regs.rcx)} * static_cast<uint32_t>(regs.rdx);
            SotcPreupload::NotePool(regs.rdi, size, static_cast<uint32_t>(caller), static_cast<uint32_t>(regs.rsi));
        }
    }
    if (!SotcNative::On() || (regs.r8 & 0x800) == 0 || regs.r9 == 0) {
        return HookAction::Continue;
    }
    uint64_t offset = 0;
    if (!CallerOffset(regs, &offset) || !InSpans(offset, kZeroCopyCallers, sizeof(kZeroCopyCallers) / sizeof(Span))) {
        return HookAction::Continue;
    }
    const uint64_t size = static_cast<uint64_t>(static_cast<uint32_t>(regs.rdx)) *
                          static_cast<uint32_t>(regs.rcx);
    AnnounceTracked(regs.r9, regs.r9 + size, g_window.zc); // announced at creation: the builder fills it afterwards
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// Chunk allocator 0x8c0d0 (Wrap). Pre: rdi = handle (the result is stored into it), esi = size (0: handle[8]).
// Post: the block is handle[0] | (handle[1] & 0xfff) << 32. Only the chunk writers' blocks are queued.
struct ChunkCall {
    uint64_t handle;
    uint32_t size;
    bool active;
};
thread_local ChunkCall t_chunk{};

HookAction ChunkPre(GuestRegs& regs, void*) {
    t_chunk.active = false;
    if (!SotcNative::On()) {
        return HookAction::Continue;
    }
    uint64_t offset = 0;
    if (!CallerOffset(regs, &offset) || !InSpans(offset, kChunkCallers, sizeof(kChunkCallers) / sizeof(Span))) {
        return HookAction::Continue;
    }
    uint32_t size = static_cast<uint32_t>(regs.rsi);
    if (size == 0) {
        size = GuestU32(regs.rdi + 0x20);
    }
    t_chunk = ChunkCall{regs.rdi, size, regs.rdi != 0 && size != 0};
    return HookAction::Continue;
}

void ChunkPost(GuestRegs&, void*) {
    if (!t_chunk.active) {
        return;
    }
    t_chunk.active = false;
    const uint64_t block = static_cast<uint64_t>(GuestU32(t_chunk.handle)) |
                           (static_cast<uint64_t>(GuestU32(t_chunk.handle + 4) & 0xfffu) << 32);
    if (block != 0) {
        ChunkPush(block, t_chunk.size);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Rewriters (Wrap). Each pre stashes the object, the post reads what the function wrote into it.

thread_local uint64_t t_object = 0;

// 0xf5cd0 (MaterialParam sub-buffer): buffer at +0x08 (guest), size u32 at +0x38 (the memcpy and the loop fill
// the whole size).
HookAction MatParamPre(GuestRegs& regs, void*) {
    t_object = SotcNative::On() ? regs.rdi : 0;
    return HookAction::Continue;
}
void MatParamPost(GuestRegs&, void*) {
    const uint64_t object = t_object;
    t_object = 0;
    if (object == 0) {
        return;
    }
    const uint64_t buffer = GuestU64(object + 0x08);
    const uint32_t size = GuestU32(object + 0x38);
    if (buffer != 0 && size != 0) {
        AnnounceRange(buffer, buffer + size, SotcNative::Kind::PerFrame, g_window.mat);
    }
}

// 0xfef40: the block at +0x48 receives the memcpy of the source header (size u32 at source+4).
void FefPost(GuestRegs&, void*) {
    const uint64_t object = t_object;
    t_object = 0;
    if (object == 0) {
        return;
    }
    const uint64_t block = GuestU64(object + 0x48);
    const uint64_t source = GuestU64(object + 0x10);
    const uint32_t size = source != 0 ? GuestU32(source + 4) : 0;
    if (block != 0 && size != 0) {
        AnnounceRange(block, block + size, SotcNative::Kind::PerFrame, g_window.blk);
    }
}
HookAction FefPre(GuestRegs& regs, void*) {
    t_object = SotcNative::On() ? regs.rdi : 0;
    return HookAction::Continue;
}

// 0x121ed0 (Entry; Wrap is refused for it): rdi = the instance object, whose persistent block at +0x1d8 is
// rewritten with 0x18 bytes each frame. Announced PerFrame before the body: the bytes are read at the next
// submit, which comes after the job waits in 0xd07c0, so the announce is safe for PerFrame data.
HookAction InstEntry(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.rdi == 0) {
        return HookAction::Continue;
    }
    const uint64_t block = GuestU64(regs.rdi + 0x1d8);
    if (block != 0) {
        AnnounceTracked(block, block + 0x18, g_window.inst, true); // announced before the body writes it
    }
    return HookAction::Continue;
}

// 0x18c390 (Entry, the only caller is 0x34ccac in sub_34cad0): rdi = the per-object binding record. Its
// +0x278..+0x298 span (position, transform, one pointer) is rewritten by the body twice per frame; the record
// is persistent (one address per session) and sits in ranges the GPU binds. PerFrame before the body: the bytes
// are read at the next submit, after the job waits in 0xd07c0.
HookAction RecordEntry(GuestRegs& regs, void*) {
    if (SotcNative::On() && regs.rdi != 0) {
        AnnounceTracked(regs.rdi + 0x278, regs.rdi + 0x298, g_window.rec, true); // before the body writes it
    }
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// Descriptor encoders (Wrap). Pre: rdi = descriptor, rsi = base, the size (count or bytes) in rcx / rdx. Post:
// the encoded word1 (stride in bits 16..29 for the buffer encoders) and word3 (format) are read back.
struct DescCall {
    uint64_t desc, base;
    uint32_t arg, arg2;
    bool active;
};
thread_local DescCall t_desc{};

HookAction DescPre(GuestRegs& regs, void*) {
    // The buffer encoders take count in rcx; the constant and image encoders take a byte size in rdx.
    t_desc = DescCall{regs.rdi, regs.rsi, static_cast<uint32_t>(regs.rdx), static_cast<uint32_t>(regs.rcx),
                      SotcNative::On() && regs.rdi != 0};
    return HookAction::Continue;
}

// Buffer V#: byte size = stride * count (stride from word1 bits 16..29); count in arg2.
void DescBufferPost(GuestRegs&, void*) {
    if (!t_desc.active) {
        return;
    }
    const uint32_t word1 = GuestU32(t_desc.desc + 4);
    const uint64_t stride = (word1 >> 16) & 0x3fff;
    const uint64_t size = stride != 0 ? stride * t_desc.arg2 : t_desc.arg2;
    SotcNative::NoteDescriptor(t_desc.desc, t_desc.base, size, GuestU32(t_desc.desc + 12));
    g_window.desc.fetch_add(1, std::memory_order_relaxed);
    t_desc.active = false;
}

// Constant and image V#: byte size in arg (rdx).
void DescSizePost(GuestRegs&, void*) {
    if (!t_desc.active) {
        return;
    }
    SotcNative::NoteDescriptor(t_desc.desc, t_desc.base, t_desc.arg, GuestU32(t_desc.desc + 12));
    g_window.desc.fetch_add(1, std::memory_order_relaxed);
    t_desc.active = false;
}

// ---------------------------------------------------------------------------------------------------------
// Particle record shift (Site at 0x101817, after the record writes of one record). r14 = source record base
// (pool + r15), [rbp-0x30] = destination base (the memcpy target), r15 = offset of the record (stride * index),
// rdi = the particle object (stride u32 at +0x74).
HookAction ParticleShift(GuestRegs& regs, void*) {
    if (!SotcNative::On()) {
        return HookAction::Continue;
    }
    const uint32_t stride = GuestU32(regs.rdi + 0x74);
    if (stride == 0) {
        return HookAction::Continue;
    }
    const uint64_t source = regs.r14 + regs.r15;
    const uint64_t dest = GuestU64(regs.rbp - 0x30) + regs.r15;
    AnnounceRange(source, source + stride, SotcNative::Kind::Persistent, g_window.part);
    AnnounceRange(dest, dest + stride, SotcNative::Kind::Persistent, g_window.part);
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// N2 completion (impl-writes3.md).

// 0x11788f, in Material Param Update 0x117160 right after `memcpy(rbx, DAT_02ed2108, [r12])` (0x11788a): rbx = the
// 0x12820 block the copy filled, [r12] = the size u32 (r12 still holds the 0x2ed2100 global at this site). The block
// is DMA'd to GPU memory by 0x16eb90 next, and it is rebuilt every frame: PerFrame.
HookAction MatCopy(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.rbx == 0) {
        return HookAction::Continue;
    }
    const uint32_t size = GuestU32(regs.r12);
    if (size != 0) {
        AnnounceRange(regs.rbx, regs.rbx + size, SotcNative::Kind::PerFrame, g_window.mat);
    }
    return HookAction::Continue;
}

// 0x8c120, the view block allocator (Wrap). Pre: rdi = the view record (block size u32 at +0x20), rsi = the handle the
// block is stored into (low word at [rsi], high 12 bits at [rsi+4], as in 0x8c0d0). Post: the block is queued in the
// chunk list. Callers kept: sub_bdfd0 (0xbdfd0-0xbe1f0) and SetViewParams (0xbe1f0-0xc0b6f); the two are the only
// direct callers of 0x8c120 (eboot_re.py callers 8c120).
constexpr Span kViewBlockCallers[] = {{0xbdfd0, 0xbe1f0}, {0xbe1f0, 0xc0b6f}};
struct ViewCall {
    uint64_t handle;
    uint32_t size;
    bool active;
};
thread_local ViewCall t_view{};

HookAction ViewBlockPre(GuestRegs& regs, void*) {
    t_view.active = false;
    if (!SotcNative::On()) {
        return HookAction::Continue;
    }
    uint64_t offset = 0;
    if (!CallerOffset(regs, &offset) ||
        !InSpans(offset, kViewBlockCallers, sizeof(kViewBlockCallers) / sizeof(Span))) {
        return HookAction::Continue;
    }
    const uint32_t size = GuestU32(regs.rdi + 0x20);
    t_view = ViewCall{regs.rsi, size, regs.rsi != 0 && size != 0};
    return HookAction::Continue;
}

void ViewBlockPost(GuestRegs&, void*) {
    if (!t_view.active) {
        return;
    }
    t_view.active = false;
    const uint64_t block = static_cast<uint64_t>(GuestU32(t_view.handle)) |
                           (static_cast<uint64_t>(GuestU32(t_view.handle + 4) & 0xfffu) << 32);
    if (block != 0) {
        ChunkPush(block, t_view.size);
    }
}

// 0xcef0d, in EndFrameView_WriteB (0xcedf0) after the 16-byte store `[r14+rbx] = rax, [r14+rbx+8] = rdx` (both exits
// reach 0xcef08). r14 = the view table of [obj+0x330], rbx = view index * 16. The table is persistent but rewritten
// every frame, so the 16 bytes are PerFrame.
HookAction ViewTable(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.r14 == 0) {
        return HookAction::Continue;
    }
    const uint64_t entry = regs.r14 + regs.rbx;
    AnnounceRange(entry, entry + 16, SotcNative::Kind::PerFrame, g_window.view);
    return HookAction::Continue;
}

// 0x18aa20, instance class 2 writer (Entry; rdi = the instance object). The body writes the 0x18-byte block at
// [this+0xa8] when that pointer is set (0x18aaaf..0x18ab5e), and it does not change the pointer. Announced at entry,
// PerFrame: the bytes are read at the next submit, after the job waits in 0xd07c0. Direct callers: UpdateGPUData_
// InstanceData (0x118210) and sub_1b32f0 (0x1b35ba); both are instance writers, so no caller filter.
HookAction InstClass2Entry(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.rdi == 0) {
        return HookAction::Continue;
    }
    const uint64_t block = GuestU64(regs.rdi + 0xa8);
    if (block != 0) {
        AnnounceTracked(block, block + 0x18, g_window.inst, true); // before the body writes it
    }
    return HookAction::Continue;
}

// 0x1b32f0 (BuildXPR), site at its return (0x1b3f97, `mov rax,r12`; r12 = the returned object). Called through
// 0x1b0980 by StaticModelSection::Init (0x19aad0, run as a job from 0x271b10): a model-section build, not a per-frame
// writer (impl-writes4.md). Its loop creates one zero-copy buffer V# per instance (0x8be60 flags 0x800, type 0x1b,
// elem 0xb0) at lVar25 = object+0x18, lVar25 += 0xb0, and writes the 0xb0 bytes after each call. [object+0x18,
// object+0x20) is the whole zero-copy backing (object+0x20 = object+0x18 + count * 0xb0). The range is announced
// at the return, after the writes, as Persistent (uploaded once). The 0x8be60 creator hook does not see this caller
// (not in kZeroCopyCallers), so the range is announced here only.
HookAction BuildXprExit(GuestRegs& regs, void*) {
    const uint64_t object = regs.r12;
    if (!SotcNative::On() || object == 0) {
        return HookAction::Continue;
    }
    const uint64_t begin = GuestU64(object + 0x18);
    const uint64_t end = GuestU64(object + 0x20);
    if (begin != 0 && end > begin) {
        AnnounceRange(begin, end, SotcNative::Kind::Persistent, g_window.zc);
    }
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// impl-writes6: cull dispatch block (Entry on 0x92b50 BPGnm_DispatchWithLabelWait; args rdi ctx, rsi buffer, rdx count,
// rcx block, r8 dword count). The call 0x928f0 (first in 0x92b50, unchanged) binds rcx as a buffer V# of r8 elements, so
// the GPU reads the block through user data. Only the caller span of BPXPR_IndexCullObjects (0x11dbe0..0x11e181, the one
// function that writes the block: lines 109-110 of its decompilation, right before the call) is announced. The other
// 15 call sites (SetViewParams, sub_ab630, ...) pass object fields whose writers were not verified, so they are not
// announced. The 0xceae0 thunk tail-jumps into 0x11dbe0 (impl-writes5's "twin" was wrong), so it needs no span.
// The foliage displacer variant 0xd8660 passes its block at [struct+0xc8] to the same dispatch (r8 dwords); the
// block is rewritten per job, so it is announced PerFrame like the BPXPR one.
constexpr Span kCullDispatchCallers[] = {{0x11dbe0, 0x11e181}, {0xd8660, 0xd892d}};
HookAction CullDispatchEntry(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.rcx == 0) {
        return HookAction::Continue;
    }
    uint64_t offset = 0;
    if (!CallerOffset(regs, &offset) ||
        !InSpans(offset, kCullDispatchCallers, sizeof(kCullDispatchCallers) / sizeof(Span))) {
        return HookAction::Continue;
    }
    const uint64_t size = static_cast<uint64_t>(static_cast<uint32_t>(regs.r8)) * 4; // dword count (5 = 0x14 bytes)
    AnnounceRange(regs.rcx, regs.rcx + size, SotcNative::Kind::PerFrame, g_window.cull);
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// SOTC_VERIFY=n1 misses (flicker brief 2): buffers that the engine fills with plain stores and the GPU binds in the same
// or the next frame, with no announce after the writes. 0x8be60 (NATIVE_ZC_CREATE) announces a zero-copy backing when
// the resource is created, which is before these builders fill it, so mode 1 never uploaded the bytes (mode 0 saw the
// write fault). Each hook announces exactly the buffers its builder wrote, PerFrame, when the builder is done.

// sub_198bb0 Site 0x199d62: the 0x804 buffer (type 0x1b, 16-byte elements, one per entry of the list at
// param_1 + 0x70 / + 0x78) was filled by the loop and r12 points at its end.
HookAction UbufFill198(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.r13 == 0 || regs.r12 == 0) {
        return HookAction::Continue;
    }
    const uint64_t begin = GuestU64(regs.r13 + 0x70), end = GuestU64(regs.r13 + 0x78);
    if (end <= begin) {
        return HookAction::Continue;
    }
    const uint64_t size = (end - begin) * 2; // (entries) * 16 bytes
    if (regs.r12 > size) {
        AnnounceRange(regs.r12 - size, regs.r12, SotcNative::Kind::PerFrame, g_window.ubuf);
    }
    return HookAction::Continue;
}

// Wrap pre of sub_1af960 / sub_1b0440: rdi = builder, [rdi + 8] = the object that receives the buffers, rsi = the
// element vector (begin [rsi], end [rsi + 8], 8 bytes per element). The buffers do not exist yet.
struct UbufCall {
    uint64_t object;
    uint64_t list_bytes; // end - begin of the element vector
};
thread_local UbufCall t_ubuf{};

HookAction UbufPre(GuestRegs& regs, void*) {
    t_ubuf = UbufCall{};
    if (!SotcNative::On() || regs.rdi == 0 || regs.rsi == 0) {
        return HookAction::Continue;
    }
    const uint64_t begin = GuestU64(regs.rsi), end = GuestU64(regs.rsi + 8);
    if (end > begin) {
        t_ubuf = UbufCall{GuestU64(regs.rdi + 8), end - begin};
    }
    return HookAction::Continue;
}

void UbufAnnounce(uint64_t object, uint64_t slot, uint64_t size) {
    const uint64_t block = GuestU64(object + slot);
    if (block != 0 && size != 0) {
        AnnounceRange(block, block + size, SotcNative::Kind::PerFrame, g_window.ubuf);
    }
}

// sub_1af960: obj+0x48 (0x10 * list_bytes), +0x50 (4 *), +0x58 (0x80 bytes, written by 0x123280), +0x60 (8 *, zeroed and
// filled per element at 0x1afb46 / 0x1afb79 / 0x123180) and +0x70 (1 *, the 0xbf800000 pairs at 0x1afbfd).
void UbufPost1AF960(GuestRegs&, void*) {
    const UbufCall call = t_ubuf;
    t_ubuf = UbufCall{};
    if (call.object == 0) {
        return;
    }
    UbufAnnounce(call.object, 0x48, call.list_bytes * 16);
    UbufAnnounce(call.object, 0x50, call.list_bytes * 4);
    UbufAnnounce(call.object, 0x58, 0x80);
    UbufAnnounce(call.object, 0x60, call.list_bytes * 8);
    UbufAnnounce(call.object, 0x70, call.list_bytes);
}

// sub_1b0440: obj+0x40 (5 dwords per element) and obj+0x78 (0x40 bytes per element, filled by sub_1b0080).
void UbufPost1B0440(GuestRegs&, void*) {
    const UbufCall call = t_ubuf;
    t_ubuf = UbufCall{};
    if (call.object == 0) {
        return;
    }
    UbufAnnounce(call.object, 0x40, (call.list_bytes / 8) * 0x14);
    UbufAnnounce(call.object, 0x78, call.list_bytes * 8);
}

// Foliage displacer per-frame block (sub_d83b0 0xd852b, sub_d8660 0xd87cf): an 8-byte block from the 0x12820 allocator
// (category DAT_02ed1f54, not the 0x8c0d0 wrapper, so NATIVE_CHUNK never sees it) that holds the buffer address the job
// binds (`*block = [struct+0xb0]`, plain store). Site after the call: rax = the block, announced PerFrame. The
// allocator recycles blocks once the per-frame heap wraps, so a block that was never announced shows the GPU a
// previous frame's V# (the "after a few seconds" symptom of the grass flicker).
HookAction DisplacerBlock(GuestRegs& regs, void*) {
    if (SotcNative::On() && regs.rax != 0) {
        ChunkPush(regs.rax, 8);
    }
    return HookAction::Continue;
}

// impl-writes6: 0x117930 (UpdateTextureStreamingGPUData, a second Material Param Update path), Site after
// `memcpy(rbx, DAT_02ed2108, [r12])` at 0x11805a: rbx = the 0x12820 block just filled, [r12] = its size (r12 still holds
// the 0x2ed2100 global). Same contract as MatCopy (0x11788f), with its own counter (strm) so a run shows whether the path
// is live: it is reached only through sub_117140 -> sub_d0710 (job UpdateTextureStreamingGPUData) when the job's [1] is 0.
HookAction MatCopyStream(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.rbx == 0) {
        return HookAction::Continue;
    }
    const uint32_t size = GuestU32(regs.r12);
    if (size != 0) {
        AnnounceRange(regs.rbx, regs.rbx + size, SotcNative::Kind::PerFrame, g_window.strm);
    }
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// Flicker brief 2 (SOTC_VERIFY=n1 misses): plain-store writers that no earlier announce covered.
//
// ResolveSunShadows sub_afcc0 and its twin sub_b0100 (per frame). Besides the constants chunk of 0x8c0d0 (queued by
// NATIVE_CHUNK, announced at the submit), they write two blocks of the 0x12820 allocator that NATIVE_CHUNK never
// sees: the 0x10-byte backing of the 0x804 zero-copy buffer (category DAT_02ed1f50, stored at [[object+0x10]+0xf28],
// written by the stores at 0xafe13/0xafe16/0xafe23) and the 0x18-byte bind block (category DAT_02ed1f54: three
// pointers, bound through 0x85ecd0 / 0x862490). The zero-copy creator hook NATIVE_ZC_CREATE was refused by the
// detour engine until NATIVE_RES_COPY left 0x8be60 ("the function has other CPU patches"), so the backing was
// never announced and the GPU bound it in the same frame; the bind block still needs this Site. Site after the last store of the 0x18-byte block: announce
// all three blocks (the chunk again, exactly 0x60 bytes written) PerFrame.
void AnnounceSunShadowBlocks(uint64_t object, uint64_t bind_block) {
    if (!SotcNative::On() || object == 0) {
        return;
    }
    const uint64_t owner = GuestU64(object + 0x10);
    const uint64_t backing = owner != 0 ? GuestU64(owner + 0xf28) : 0;
    AnnounceRange(backing, backing + 0x10, SotcNative::Kind::PerFrame, g_window.sun);
    AnnounceRange(bind_block, bind_block + 0x18, SotcNative::Kind::PerFrame, g_window.sun);
    const uint64_t constants = GuestU64(object + 0xb0);
    AnnounceRange(constants, constants + 0x60, SotcNative::Kind::PerFrame, g_window.sun);
}

// 0xaff60 in sub_afcc0: r15 = the shadow object (param_2), r12 = the 0x18-byte bind block.
HookAction SunShadowAfcc0(GuestRegs& regs, void*) {
    AnnounceSunShadowBlocks(regs.r15, regs.r12);
    return HookAction::Continue;
}

// 0xb03bd in sub_b0100: r14 = the shadow object (param_2), rbx = the 0x18-byte bind block.
HookAction SunShadowB0100(GuestRegs& regs, void*) {
    AnnounceSunShadowBlocks(regs.r14, regs.rbx);
    return HookAction::Continue;
}

// User-data packets. The packet writer 0x884240 (SET_SH_REG with a 2-dword marker, compute user data) cannot be
// hooked (its jump table is an unresolved indirect branch), and the writes into the command buffer reached the GPU
// before the submit announce of the DCB/ACE range (n1verify: rip 0x884289, binds <= 64 KiB). Its callers 0x85ecd0
// (tail jump) and 0x85e5d0 (user-data flush) take the command buffer object from [rdi+0x90]; its cursor is at +0x10
// (the buffers are fixed-size: the refill callback returns 0). Pre/post read the cursor and announce exactly the
// words written in between, PerFrame, right after they are written.
struct CursorCall {
    uint64_t object;
    uint64_t cursor;
};
thread_local CursorCall t_udata{};

HookAction UdataPre(GuestRegs& regs, void*) {
    t_udata = CursorCall{};
    if (!SotcNative::On() || regs.rdi == 0) {
        return HookAction::Continue;
    }
    const uint64_t object = GuestU64(regs.rdi + 0x90);
    if (object != 0) {
        t_udata = CursorCall{object, GuestU64(object + 0x10)};
    }
    return HookAction::Continue;
}

void UdataPost(GuestRegs&, void*) {
    const CursorCall call = t_udata;
    t_udata = CursorCall{};
    if (call.object == 0 || call.cursor == 0) {
        return;
    }
    const uint64_t cursor = GuestU64(call.object + 0x10);
    if (cursor > call.cursor && cursor - call.cursor <= (1u << 16)) {
        AnnounceRange(call.cursor, cursor, SotcNative::Kind::PerFrame, g_window.pkt);
    }
}

// 0x884349, the end of the packet writer 0x884240 (site; the jump table rules out a wrap): the packet is complete,
// rcx = its start + 8 and edx = the dwords after those 8 bytes. Covers the callers without a cursor hook (17 of 19;
// after the opening cinematic the game binds through them and their packets reached the GPU stale: no grass).
// 0xf9c40, the instance staging copy (callers 0xfb1e0 and 0xfb2d0): 32 bytes per dirty object into
// [rdi+0x580] + ([rdi+0x478] - [rdi+0x470]); the count is the high dword of [rdi+0x5b0] (none when its low dword is 0).
// Pre reads the destination and count, post announces the records, PerFrame. Unannounced before: after the opening
// cinematic the Shrine terrain was drawn from stale instance bytes.
struct StageCall {
    uint64_t begin;
    uint64_t end;
};
thread_local StageCall t_stage{};

HookAction InstStagePre(GuestRegs& regs, void*) {
    t_stage = StageCall{};
    if (!SotcNative::On() || regs.rdi == 0) {
        return HookAction::Continue;
    }
    const uint64_t obj = regs.rdi;
    const uint64_t word = GuestU64(obj + 0x5b0);
    const auto count = static_cast<int32_t>(word >> 32);
    if (static_cast<uint32_t>(word) == 0 || count <= 0 || count > (1 << 20)) {
        return HookAction::Continue;
    }
    const uint64_t dest = GuestU64(obj + 0x580) + (GuestU64(obj + 0x478) - GuestU64(obj + 0x470));
    t_stage = StageCall{dest, dest + uint64_t{0x20} * static_cast<uint64_t>(count)};
    return HookAction::Continue;
}

void InstStagePost(GuestRegs&, void*) {
    const StageCall call = t_stage;
    t_stage = StageCall{};
    if (call.begin != 0 && call.end > call.begin) {
        AnnounceRange(call.begin, call.end, SotcNative::Kind::PerFrame, g_window.pkt);
    }
}

HookAction UdataPacketEnd(GuestRegs& regs, void*) {
    if (!SotcNative::On() || regs.rcx < 8) {
        return HookAction::Continue;
    }
    const uint64_t start = regs.rcx - 8;
    const uint64_t end = regs.rcx + (regs.rdx & 0xffffffffull) * 4;
    if (end > start && end - start <= 0x100) {
        AnnounceRange(start, end, SotcNative::Kind::PerFrame, g_window.pkt);
    }
    return HookAction::Continue;
}

// 0xa3ee50, the cloth skeleton matrix palette builder (callers sub_a3e170 and sub_a3fa30; the result is stored at
// object+0xe0 or passed to the buffer creator). The index vector at param_1+0x90..0x98 holds 4-byte joint indices;
// the block returned by 0x12820 is (bytes * 16) = joints * 0x40 bytes, every 0x40 bytes a 4x4 matrix written by plain
// stores (n1verify: rip 0xa3ef1b). Complete at return: announced PerFrame there.
thread_local uint64_t t_palette_bytes = 0;

HookAction PalettePre(GuestRegs& regs, void*) {
    t_palette_bytes = 0;
    if (SotcNative::On() && regs.rdi != 0) {
        const uint64_t begin = GuestU64(regs.rdi + 0x90), end = GuestU64(regs.rdi + 0x98);
        if (end > begin && end - begin <= (1u << 20)) {
            t_palette_bytes = (end - begin) * 16;
        }
    }
    return HookAction::Continue;
}

void PalettePost(GuestRegs& regs, void*) {
    const uint64_t bytes = t_palette_bytes;
    t_palette_bytes = 0;
    if (bytes != 0 && regs.rax != 0) {
        AnnounceRange(regs.rax, regs.rax + bytes, SotcNative::Kind::PerFrame, g_window.pal);
    }
}

// Earlier batch: resource copies and mip streaming (unchanged).

// 0x96b59, in the texture build 0x96680 after the single-block texel copy: r14 = the block from
// 0x12820 (destination), rbx = its size.
HookAction TexCopy(GuestRegs& regs, void*) {
    if (SotcNative::On() && regs.r14 != 0 && regs.rbx != 0) {
        SotcNative::Announce(regs.r14, regs.rbx, SotcNative::Kind::Persistent);
        Count(g_window.tex_announces, g_window.tex_bytes, regs.rbx);
    }
    return HookAction::Continue;
}

// 0x96b1b, in 0x96680 after the mip-tail texel copy: r13 = destination (pool base + offset),
// r12 = the copy's accounted size (the same value the add at this site adds to the pool counter).
HookAction TexTailCopy(GuestRegs& regs, void*) {
    if (SotcNative::On() && regs.r13 != 0 && regs.r12 != 0) {
        SotcNative::Announce(regs.r13, regs.r12, SotcNative::Kind::Persistent);
        Count(g_window.tex_announces, g_window.tex_bytes, regs.r12);
    }
    return HookAction::Continue;
}

// 0x98d6c, in the streamer 0x98ae0 after the mip memcpy: r13 = pool destination, r14 = mip size.
HookAction MipCopy(GuestRegs& regs, void*) {
    if (SotcNative::On() && regs.r13 != 0 && regs.r14 != 0) {
        SotcNative::Announce(regs.r13, regs.r14, SotcNative::Kind::Persistent);
        Count(g_window.mip_announces, g_window.mip_bytes, regs.r14);
    }
    return HookAction::Continue;
}

// 0x98be0, in 0x98ae0 before the mode-2 decommit of a dropped mip: rdi = pool base, r14 = size.
HookAction MipRelease(GuestRegs& regs, void*) {
    if (SotcNative::On() && regs.rdi != 0 && regs.r14 != 0) {
        SotcNative::Release(regs.rdi, regs.r14);
        Count(g_window.mip_releases, g_window.mip_release_bytes, regs.r14);
    }
    return HookAction::Continue;
}

// ---------------------------------------------------------------------------------------------------------
// impl-writes7 gap 2: guest memset/memcpy/memmove (src/runtime.c) and file reads (src/runtime_file.c) announce the
// bytes they put into guest memory. The runtime calls these after the copy has landed, so the announce is post-write,
// as the Site hooks are. Every guest copy is announced PerFrame (a page mark: nothing is uploaded until a bind reads
// the range, and a range the GPU wrote is copied byte-exact, BufferCache::NativeDrain). A table of three callers
// whose destination was known GPU data missed the engine's other copies of GPU data: without page tracking the
// Shrine of Worship facade was never uploaded (1 Nov). Announcing every copy costs nothing measurable on the Lab
// route (~130k announces/s, same FPS and p99).
// The code of the announce a guest copy needs: 0 = none, else 1 + SotcNative::Kind. Lock-free and cheap: it runs
// on every guest memcpy/memset/memmove.
// sotc: SOTC_PRELOAD_UPLOAD. The return address of the initial-data memcpy in the resource creator 0x8be60 (call at
// 0x8bf4b, EU 1.01 / US 1.00): the GPU allocation it just filled from the loader's stream (vertex, index and texel
// buffers built at load). Flag bit of the code (the kind stays PerFrame).
constexpr uint64_t kResCopyReturn = 0x8bf50;
constexpr uint32_t kResCopyFlag = 0x100;

uint32_t GuestWriteCode(uint64_t return_address) {
    if (!SotcNative::On()) {
        return 0u;
    }
    uint32_t code = 1u + static_cast<uint32_t>(SotcNative::Kind::PerFrame);
    if (SotcPreupload::On()) {
        const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
        if (base != 0 && return_address == base + kResCopyReturn) {
            code |= kResCopyFlag;
        }
    }
    return code;
}

// Announces [dst, dst + size) for a guest copy that has landed.
void AnnounceWrite(uint64_t dst, uint64_t size, uint32_t code) {
    if (code == 0 || size == 0 || dst > UINT64_MAX - size) {
        return;
    }
    if (code & kResCopyFlag) {
        code &= ~kResCopyFlag;
        SotcPreupload::NoteLoaded(dst, size, SotcPreupload::kResCopy, t_res_create.caller, t_res_create.kind);
    }
    AnnounceRange(dst, dst + size, static_cast<SotcNative::Kind>(code - 1), g_window.gmem);
}

} // namespace

// Report lines for the 5 s window (SotcNative::Report prints the rest). Resets the counters.
void Report(std::string& out, double seconds) {
    const auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const uint64_t ra = take(g_window.res_announces), rb = take(g_window.res_bytes);
    const uint64_t rg = take(g_window.res_gpu_bytes), rc = take(g_window.res_cpu_bytes);
    const uint64_t ta = take(g_window.tex_announces), tb = take(g_window.tex_bytes);
    const uint64_t ma = take(g_window.mip_announces), mb = take(g_window.mip_bytes);
    const uint64_t mr = take(g_window.mip_releases), mrb = take(g_window.mip_release_bytes);
    const uint64_t sk = take(g_window.skipped);
    const double mib = 1024.0 * 1024.0;
    char line[640];
    std::snprintf(line, sizeof(line),
                  "SOTC NATIVE WRITES res %llu announces %.2f MiB (gpu %.2f cpu %.2f) tex %llu %.2f MiB "
                  "mip %llu %.2f MiB rel %llu %.2f MiB skipped %llu per %.1f s\n",
                  static_cast<unsigned long long>(ra), rb / mib, rg / mib, rc / mib,
                  static_cast<unsigned long long>(ta), tb / mib, static_cast<unsigned long long>(ma),
                  mb / mib, static_cast<unsigned long long>(mr), mrb / mib,
                  static_cast<unsigned long long>(sk), seconds);
    out += line;

    const auto cls = [&](const char* name, Counter& c) {
        const uint64_t n = c.Take(c.n), b = c.Take(c.bytes);
        std::snprintf(line, sizeof(line), " %s %llu %.2f MiB", name, static_cast<unsigned long long>(n), b / mib);
        return std::string(line);
    };
    out += "SOTC NATIVE WRITES PF";
    out += cls("dcb", g_window.dcb);
    out += cls("ccb", g_window.ccb);
    out += cls("chunk", g_window.chunk);
    out += cls("zc", g_window.zc);
    out += cls("mat", g_window.mat);
    out += cls("blk", g_window.blk);
    out += cls("inst", g_window.inst);
    out += cls("part", g_window.part);
    out += cls("view", g_window.view);
    out += cls("strm", g_window.strm);
    out += cls("cull", g_window.cull);
    out += cls("rec", g_window.rec);
    out += cls("ubuf", g_window.ubuf);
    out += cls("gmem", g_window.gmem);
    out += cls("file", g_window.file);
    out += cls("ring", g_window.ring);
    out += cls("sun", g_window.sun);
    out += cls("pkt", g_window.pkt);
    out += cls("pal", g_window.pal);
    std::snprintf(line, sizeof(line), " desc %llu dropped %llu\n",
                  static_cast<unsigned long long>(take(g_window.desc)),
                  static_cast<unsigned long long>(take(g_window.chunk_dropped)));
    out += line;
}

void Register() {
    using SotcHooks::Handler;
    SotcHooks::RegisterHandler("NATIVE_TEX_COPY", Handler{TexCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_TEX_TAIL_COPY", Handler{TexTailCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MIP_COPY", Handler{MipCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MIP_RELEASE", Handler{MipRelease, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DCB_SUBMIT", Handler{DcbSubmit, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_CCB_SUBMIT", Handler{CcbSubmit, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DCB_SUBMIT_FLIP", Handler{DcbSubmitFlip, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_ZC_CREATE", Handler{ZeroCopyCreate, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_CHUNK", Handler{ChunkPre, ChunkPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MATPARAM", Handler{MatParamPre, MatParamPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_FEF40_BLOCK", Handler{FefPre, FefPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_INST_121ED0", Handler{InstEntry, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DESC_865410", Handler{DescPre, DescBufferPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DESC_8655D0", Handler{DescPre, DescSizePost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DESC_865600", Handler{DescPre, DescSizePost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DESC_865500", Handler{DescPre, DescBufferPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_PARTICLE_SHIFT", Handler{ParticleShift, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MAT_COPY", Handler{MatCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_VIEW_BLOCK", Handler{ViewBlockPre, ViewBlockPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_VIEW_TABLE", Handler{ViewTable, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_INST_18AA20", Handler{InstClass2Entry, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_BUILDXPR_ZC", Handler{BuildXprExit, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_CULL_DISPATCH_BLK", Handler{CullDispatchEntry, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MAT_COPY_STREAM", Handler{MatCopyStream, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_RECORD_18C390", Handler{RecordEntry, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_UBUF_198BB0", Handler{UbufFill198, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_UBUF_1AF960", Handler{UbufPre, UbufPost1AF960, nullptr});
    SotcHooks::RegisterHandler("NATIVE_UBUF_1B0440", Handler{UbufPre, UbufPost1B0440, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DISP_BLOCK_D83B0", Handler{DisplacerBlock, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DISP_BLOCK_D8660", Handler{DisplacerBlock, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_SUNSHADOW_AFCC0", Handler{SunShadowAfcc0, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_SUNSHADOW_B0100", Handler{SunShadowB0100, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_UDATA_85ECD0", Handler{UdataPre, UdataPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_UDATA_85E5D0", Handler{UdataPre, UdataPost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_UDATA_884349", Handler{UdataPacketEnd, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_INST_STAGE_F9C40", Handler{InstStagePre, InstStagePost, nullptr});
    SotcHooks::RegisterHandler("NATIVE_PALETTE_A3EE50", Handler{PalettePre, PalettePost, nullptr});
}

// The RenderSingleFrameCPU ring (memtype 17): every block of it is per-frame GPU data written by the render jobs
// (impl-writes7-region.md: 0xc7850, RunFunctionAsRenderJob). The blocks go on the chunk list and are announced
// PerFrame at the next submit, after every writer of the frame. Called by the MEMTYPE_ALLOC post phase
// (probe_memtype.cpp).
void NoteFrameBlock(uint64_t addr, uint64_t size) {
    if (!SotcNative::On() || addr == 0 || size == 0 || size > kMaxRange) {
        return;
    }
    ChunkPush(addr, static_cast<uint32_t>(size));
}

} // namespace SotcNativeHooks

// C entry points for the runtime (src/runtime.c), declared in gpu/bbgpu.h. Lookups are
// lock-free; bbgpu_native_announce_write takes the SotcNative mutex, so the runtime calls it on the host stack.
extern "C" uint32_t bbgpu_native_guest_write_kind(uint64_t return_address) {
    return SotcNativeHooks::GuestWriteCode(return_address);
}

extern "C" void bbgpu_native_announce_write(void* dst, uint64_t size, uint32_t code) {
    if (!SotcNative::On()) {
        return;
    }
    SotcNativeHooks::AnnounceWrite(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst)), size, code);
}

extern "C" void bbgpu_native_announce_file_read(void* dst, uint64_t size) {
    const uint64_t first = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst));
    if (!SotcNative::On() || size == 0 || first > UINT64_MAX - size) {
        return;
    }
    SotcNativeHooks::AnnounceRange(first, first + size, SotcNative::Kind::PerFrame, SotcNativeHooks::g_window.file);
}

namespace SotcNativeRing {

// sotc: the ACE ring dwords of one sceGnmDingDong (video_core/sotc_native_ring.h). The IB packets are written by
// the engine's compute ring writer (eboot 0x884c40) and read in place by the port after the doorbell, so they
// are PerFrame data like the DCB and CCB ranges. Same guards as the other announces (AnnounceRange).
void Announce(const void* begin, std::size_t bytes) {
    if (!SotcNative::On() || begin == nullptr || bytes == 0) {
        return;
    }
    const uint64_t first = reinterpret_cast<uintptr_t>(begin);
    SotcNativeHooks::AnnounceRange(first, first + bytes, SotcNative::Kind::PerFrame,
                                   SotcNativeHooks::g_window.ring);
}

} // namespace SotcNativeRing

