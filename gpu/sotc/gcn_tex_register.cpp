// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_TEX_REGISTER hook handlers (gcn_tex_register.h). Every handler runs on a game thread: it only reads
// guest memory (backing view, range-checked: a freed object reads as garbage, never as a fault) and queues words for
// the draw thread (SotcGcn::Tex, gcn_tex.cpp). No texture cache work and no log here.
//
// Engine facts (docs/re-stutters/native/re-pipelines/texture-binding.md T7-T9, docs/re-backend/08-texture-lifecycle.md):
// - material blocks (MatTexBlock): block 1 at material+0x578, block 2 at +0x5b8; in a block, +0x00 the payload array
//   (one CTexture* per list entry), +0x08 the host copy of the GPU data (blob bytes, then one 32-byte T# per entry),
//   +0x38 the size (blob bytes + 32 * count), +0x3c the count. The T# of entry i is at data + size - 32 * count + 32i
//   (S % 16 == 0 in every material of the 4 Nov run, so the build and refresh placements agree);
// - a CTexture's T# is at +0xc4 (32 bytes); its base is written by 0x881e60 and its levels by 0x880c30 (creation and
//   streaming), in place;
// - render targets are CTextures made by 0x97a50 (from render_targets_allocate 0x9aef0);
// - pass textures are copied per bind by the T# setters 0x861fe0 (layout table +0x22) and 0x862160 (+0x42):
//   (ctx, stage, first slot, count, data = count T#s of 32 bytes).
#include "gcn_tex_register.h"

#include <cstring>

#include "guest_mem.h"
#include "hooks.h"
#include "video_core/sotc_arch/gcn_native.h"

namespace SotcGcnTexRegister {
namespace {

using SotcGcn::Tex::Source;
using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kMatBlock1 = 0x578;
constexpr uint64_t kMatBlock2 = 0x5b8;
constexpr uint64_t kBlockPayload = 0x00;
constexpr uint64_t kBlockData = 0x08;
constexpr uint64_t kBlockSize = 0x38;
constexpr uint64_t kBlockCount = 0x3c;
constexpr uint64_t kTSharp = 0xc4; // CTexture: T# (32 bytes)
constexpr uint32_t kMaxEntries = 64;      // a list longer than this is not a material block read correctly
constexpr uint32_t kMaxBlob = 0x4000;     // blob bytes before the T#s (S1 0..560 and S2 0..64 in the 4 Nov run)
constexpr uint32_t kMaxPassCount = 16;    // T#s per setter call read at most

using Words = uint32_t[8];

bool ReadWords(uint64_t address, Words& out) {
    return SotcGuest::Read(address, out, sizeof(Words));
}

// One block of a material: the T#s the shader reads (the block data) and, when they differ, the T#s of the list
// payloads (the CTextures the block was built from; equal unless a refresh has not reached the block yet).
void ReadBlock(uint64_t block) {
    uint32_t size = 0;
    uint32_t count = 0;
    uint64_t data = 0;
    uint64_t payloads = 0;
    if (!SotcGuest::Read(block + kBlockSize, &size) || !SotcGuest::Read(block + kBlockCount, &count) ||
        !SotcGuest::Read(block + kBlockData, &data) || !SotcGuest::Read(block + kBlockPayload, &payloads)) {
        return;
    }
    // A block with blob bytes and no entries has size > 0 and count 0 (T8.1): nothing to register.
    if (count == 0 || count > kMaxEntries || size < 32 * count || size - 32 * count > kMaxBlob || data == 0) {
        return;
    }
    const uint64_t first = data + (size - 32 * count);
    for (uint32_t i = 0; i < count; ++i) {
        Words block_words{};
        const bool have_block = ReadWords(first + 32ull * i, block_words);
        if (have_block) {
            SotcGcn::Tex::QueueRegister(block_words, Source::Material);
        }
        uint64_t payload = 0;
        Words payload_words{};
        if (payloads && SotcGuest::Read(payloads + 8ull * i, &payload) && payload &&
            ReadWords(payload + kTSharp, payload_words) &&
            (!have_block || std::memcmp(block_words, payload_words, sizeof(Words)) != 0)) {
            SotcGcn::Tex::QueueRegister(payload_words, Source::Material);
        }
    }
}

// GCN_TEX_REG_BASE: wrap of 0x881e60 (rdi = CTexture + 0xc4). The pre phase keeps the address and the words before
// the call (a small per-thread stack: the wrapped function is not re-entered, but nothing relies on that).
struct BaseFrame {
    uint64_t addr;
    Words old;
};
constexpr int kBaseDepth = 8;
thread_local BaseFrame t_base[kBaseDepth];
thread_local int t_base_depth = 0;

HookAction PreBase(GuestRegs& regs, void*) {
    const int depth = t_base_depth++;
    if (depth < kBaseDepth) {
        BaseFrame& f = t_base[depth];
        f.addr = regs.rdi;
        if (!SotcGcn::Tex::RegisterOn(Source::Refresh) || !ReadWords(f.addr, f.old)) {
            f.addr = 0;
        }
    }
    return HookAction::Continue;
}

void PostBase(GuestRegs&, void*) {
    if (t_base_depth <= 0) {
        return;
    }
    const int depth = --t_base_depth;
    if (depth >= kBaseDepth || t_base[depth].addr == 0) {
        return;
    }
    const BaseFrame& f = t_base[depth];
    Words now{};
    if (ReadWords(f.addr, now)) {
        SotcGcn::Tex::QueueRefresh(f.addr, f.old, now);
    }
}

// GCN_TEX_REG_LEVEL: site at 0x880c46 inside 0x880c30 (the level writer), before `and eax, 0xfff00fff`:
// rdi = the T#, eax = its dword 3 (read from [rdi+0xc]), esi = base level, edx = last level (not masked yet). The
// function then stores (eax & 0xfff00fff) | (esi & 0xf) << 12 | (edx & 0xf) << 16 to dword 3.
HookAction PreLevel(GuestRegs& regs, void*) {
    if (!SotcGcn::Tex::RegisterOn(Source::Refresh)) {
        return HookAction::Continue;
    }
    Words old{};
    if (!ReadWords(regs.rdi, old)) {
        return HookAction::Continue;
    }
    Words now{};
    std::memcpy(now, old, sizeof(Words));
    const auto dw3 = static_cast<uint32_t>(regs.rax);
    now[3] = (dw3 & 0xfff00fffu) | ((static_cast<uint32_t>(regs.rsi) & 0xfu) << 12) |
             ((static_cast<uint32_t>(regs.rdx) & 0xfu) << 16);
    SotcGcn::Tex::QueueRefresh(regs.rdi, old, now);
    return HookAction::Continue;
}

// GCN_TEX_REG_RT: wrap of 0x97a50 (descriptor / render target factory); the post phase has the CTexture in rax (0 on
// failure), its T# complete.
void PostRenderTarget(GuestRegs& regs, void*) {
    const uint64_t object = regs.rax;
    if (!object) {
        return;
    }
    SotcGcn::Tex::NoteRenderTarget(object); // SOTC_GCN_TEX_REGISTER_LOG only
    if (!SotcGcn::Tex::RegisterOn(Source::RenderTarget)) {
        return;
    }
    Words words{};
    if (ReadWords(object + kTSharp, words)) {
        SotcGcn::Tex::QueueRegister(words, Source::RenderTarget);
    }
}

// GCN_TEX_REG_PASS_T22 / _T42: entry of the T# setters 0x861fe0 / 0x862160: ecx = count, r8 = count T#s. Called per
// bind; QueueRegister drops a T# seen recently without taking a lock.
HookAction PrePass(GuestRegs& regs, void*) {
    if (!SotcGcn::Tex::RegisterOn(Source::Pass)) {
        return HookAction::Continue;
    }
    const auto count = static_cast<uint32_t>(regs.rcx);
    const uint64_t data = regs.r8;
    if (!data || count == 0 || count > kMaxPassCount) {
        return HookAction::Continue;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Words words{};
        if (ReadWords(data + 32ull * i, words)) {
            SotcGcn::Tex::QueueRegister(words, Source::Pass);
        }
    }
    return HookAction::Continue;
}

void Add(const char* name, SotcHooks::PreHandler pre, SotcHooks::PostHandler post) {
    SotcHooks::Handler handler;
    handler.pre = pre;
    handler.post = post;
    SotcHooks::RegisterHandler(name, handler);
}

} // namespace

bool MaterialsWanted() noexcept {
    static const bool wanted = SotcGcn::Tex::RegisterRequested(Source::Material);
    return wanted;
}

void OnMaterial(uintptr_t material) {
    if (!material || !SotcGcn::Tex::RegisterOn(Source::Material)) {
        return;
    }
    ReadBlock(material + kMatBlock1);
    ReadBlock(material + kMatBlock2);
}

void Register() {
    Add("GCN_TEX_REG_BASE", PreBase, PostBase);
    Add("GCN_TEX_REG_LEVEL", PreLevel, nullptr);
    Add("GCN_TEX_REG_RT", nullptr, PostRenderTarget);
    Add("GCN_TEX_REG_PASS_T22", PrePass, nullptr);
    Add("GCN_TEX_REG_PASS_T42", PrePass, nullptr);
}

} // namespace SotcGcnTexRegister
