// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native write announces (docs/re-stutters/native/PLAN.md N2, declarations in
// scripts/hooks.d/native_writes.py, switch SOTC_NATIVE). Every hook sits after the engine finished a write (or
// before a release): Entry hooks on submits and creators read the arguments the writer already filled, Wrap
// hooks announce in the post phase, Site hooks sit after the store. Handlers only read registers and the guest
// words they name, then call SotcNative::Announce / Release / NoteDescriptor (native.h). Every handler returns
// Continue: the displaced instructions or the function body run next, unchanged.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../hooks.h"
#include "native.h"

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
    Counter dcb, ccb, chunk, zc, mat, blk, inst, part, view, strm, cull;
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
    SotcNative::Announce(begin, end - begin, kind);
    c.Add(end - begin);
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
    std::vector<ChunkBlock> batch;
    {
        std::lock_guard<std::mutex> lock(g_chunk_mutex);
        batch.swap(g_chunks);
    }
    for (const ChunkBlock& b : batch) {
        AnnounceRange(b.addr, b.addr + b.size, SotcNative::Kind::PerFrame, g_window.chunk);
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
HookAction ZeroCopyCreate(GuestRegs& regs, void*) {
    if (!SotcNative::On() || (regs.r8 & 0x800) == 0 || regs.r9 == 0) {
        return HookAction::Continue;
    }
    uint64_t offset = 0;
    if (!CallerOffset(regs, &offset) || !InSpans(offset, kZeroCopyCallers, sizeof(kZeroCopyCallers) / sizeof(Span))) {
        return HookAction::Continue;
    }
    const uint64_t size = static_cast<uint64_t>(static_cast<uint32_t>(regs.rdx)) *
                          static_cast<uint32_t>(regs.rcx);
    AnnounceRange(regs.r9, regs.r9 + size, SotcNative::Kind::PerFrame, g_window.zc);
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
        AnnounceRange(block, block + 0x18, SotcNative::Kind::PerFrame, g_window.inst);
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
        AnnounceRange(block, block + 0x18, SotcNative::Kind::PerFrame, g_window.inst);
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
constexpr Span kCullDispatchCallers[] = {{0x11dbe0, 0x11e181}};
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

// Earlier batch: resource copies and mip streaming (unchanged).

// 0x8bf50, inside the resource buffer wrapper 0x8be60 after `memcpy(r15, r14, [rbx+0x20])`.
// r15 = the new block (the copy's destination), rbx = the resource object (size u32 at +0x20),
// rbp-0x34 = the flags word (param_5): bit 1 or flags 0x18 with bit 0 clear select GPUMemory.
// Zero-copy (0x800) never reaches this site. Persistent: uploaded once, then read-only.
HookAction ResCopy(GuestRegs& regs, void*) {
    if (!SotcNative::On()) {
        return HookAction::Continue;
    }
    const uint64_t dest = regs.r15;
    const uint64_t size = GuestU32(regs.rbx + 0x20);
    if (dest == 0 || size == 0) {
        g_window.skipped.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    SotcNative::Announce(dest, size, SotcNative::Kind::Persistent);
    Count(g_window.res_announces, g_window.res_bytes, size);
    const uint32_t flags = GuestU32(regs.rbp - 0x34);
    const bool gpu = (flags & 2) == 0 && ((flags & 0x18) != 0 || (flags & 1) == 0);
    (gpu ? g_window.res_gpu_bytes : g_window.res_cpu_bytes).fetch_add(size, std::memory_order_relaxed);
    return HookAction::Continue;
}

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
    std::snprintf(line, sizeof(line), " desc %llu dropped %llu\n",
                  static_cast<unsigned long long>(take(g_window.desc)),
                  static_cast<unsigned long long>(take(g_window.chunk_dropped)));
    out += line;
}

void Register() {
    using SotcHooks::Handler;
    SotcHooks::RegisterHandler("NATIVE_RES_COPY", Handler{ResCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_TEX_COPY", Handler{TexCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_TEX_TAIL_COPY", Handler{TexTailCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MIP_COPY", Handler{MipCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MIP_RELEASE", Handler{MipRelease, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_DCB_SUBMIT", Handler{DcbSubmit, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_CCB_SUBMIT", Handler{CcbSubmit, nullptr, nullptr});
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
}

} // namespace SotcNativeHooks

