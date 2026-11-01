// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native XPR visibility masks. See native_masks.h; facts from the mask RE of 1 Nov (impl-masks.md).
//
// Producer: BPXPR_BuildSetDraws (0x19b760) allocates four masks of 4 * ceil(n / 32) bytes per set (n = u16 at
// [owner+0x18]+0x64), records the cull dispatch (0x86b9e0 at 0x19c0f1), then stores the four pointers (kept at
// [rsp+0x140..+0x158]) into the set table entry *(owner+0x148). NATIVE_MASK_PRODUCE is the site right after the
// dispatch call (0x19c0f6): the four masks are registered there, key = (owner, slot), seq = the native frame. The
// command processor sees the dispatch later (after the frame's submit), so the copy follows it in the stream.
// The shadow producer sub_19c1a0 (from XPRRunFragmentCullShaderShadowScenes 0xc6bd0) does the same with two
// masks per call ([rbp-0x368], [rbp-0x360], stored at entry +0x20 / +0x60 + 8 * index, index = the int at
// [rbp-0x348]); NATIVE_MASK_PRODUCE_SHADOW is the site after its dispatch (0x19c6b1), owner in r12.
//
// Consumer: the game reads each mask in the same frame as the cull that writes it, after a label spin on the
// cull's EOP. With the spins removed, the newest completed copy of the same (owner, slot) is served: the previous
// frame's mask in practice (this frame's cull is not done yet). A set seen for the first time gets a zero mask for
// that frame (nothing drawn: one frame late, never the all-visible set that overflows the command buffer).
//
// Ordering: the consumer's label spin (RecordMaskedDraws: a busy loop, or JobSystem_WaitForCounter, until the
// qword label is 1) waits for the cull's EOP, which also orders it after the CPU job that records the cull and
// stores the mask pointers and draw records (BPXPR_RunCullPasses). The spin stays; only its GPU part goes:
// XPRPreRender (0xca6d0) resets the two cull labels (obj+0x40 shadow, obj+0x68 main) before it queues the job,
// and when the job calls BPGnm_WriteCsDoneLabel (0x925c0) for one of them the port stores 1 in it at once (the
// GPU's EOP stores the same 1 later). So the spin ends when the cull is recorded, never waiting for the GPU.
// (1 Nov: removing the spins instead let the consumers read draw records the job had not written yet.)
//
// Decals (sub_19c7e0, called per decal owner from the decal cull sub_c5200, consumer sub_1a8a40 from sub_19c740):
// the producer is NATIVE_MASK_PRODUCE_DECAL, the site right after the cull dispatch (0x19ce08). The owner (decal
// entry) is at [rbp-0x340], the mask pointer at [rbp-0x358] (the site stores it at entry+0xa0 next). The key is
// (owner, kDecalSlot): slot 32 is outside the main (0..3) and shadow (4..19) slots. The decal cull label obj+0x78
// (obj = [cull+0xe8]) is reset by NATIVE_DECAL_RESET (sub_c53d0, 0xc54c7) and stored to 1 at record time by the same
// NATIVE_CULL_RECORDED path as the main labels. RenderXPRDecals (0x11e190) spins on that label before the consumer
// loop, so the decal spin also ends at record time.
#include "native_masks.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "../guest_mem.h"
#include "../hooks.h"
#include "native.h"
#include "native_copies.h"

// src/runtime_memory.c: a guest-visible block in the low range (weak: test binaries without the loader).
extern "C" void* runtime_low_map(size_t size, int prot) __attribute__((weak));

namespace SotcNativeMasks {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kProduceHook = "NATIVE_MASK_PRODUCE";
constexpr const char* kProduceShadowHook = "NATIVE_MASK_PRODUCE_SHADOW";
constexpr const char* kProduceDecalHook = "NATIVE_MASK_PRODUCE_DECAL";
constexpr const char* kDecalResetHook = "NATIVE_DECAL_RESET";
constexpr int64_t kDecalOwnerSlot = -0x340;      // [rbp-0x340]: the decal owner (entry) of sub_19c7e0 at the site
constexpr int64_t kDecalMaskSlot = -0x358;       // [rbp-0x358]: the mask pointer the site stores at entry+0xa0
constexpr uint64_t kDecalSlot = 32;              // the key slot of the decal mask (main 0..3, shadow 4..19)
constexpr uint64_t kDecalObjectOffset = 0x20;    // owner+0x20: the decal object; its u32 count at +0
constexpr uint64_t kDecalLabel = 0x78;           // obj+0x78, obj = [cull+0xe8] (rax at the reset site)
constexpr int64_t kShadowMaskA = -0x368;         // [rbp-0x368], [rbp-0x360]: the two shadow masks at the site
constexpr int64_t kShadowMaskB = -0x360;
constexpr int64_t kShadowIndex = -0x348;         // int: the shadow slot index of the call
constexpr uint64_t kMaxShadowIndex = 8;
constexpr int kProtReadWrite = 1 | 2;            // PROT_READ | PROT_WRITE (src/compat_win.h)
constexpr uint64_t kOwnerSetOffset = 0x18;       // owner+0x18: the set record (its u16 count at +0x64)
constexpr uint64_t kSetCountOffset = 0x64;
constexpr uint64_t kMaskStackOffset = 0x140;     // [rsp+0x140..+0x158]: the four mask pointers at the site
constexpr uint64_t kMaxMaskBytes = 8 * 1024;     // 65535 bits rounded up to qwords
constexpr uint64_t kServeBlocks = 64;            // consumers read their block during the call only
constexpr uint64_t kOldestFrames = 3;            // serve this frame .. three frames back

std::atomic<uint64_t> g_next_block{0};

constexpr const char* kCullResetHook = "NATIVE_CULL_RESET";
constexpr const char* kCullRecordedHook = "NATIVE_CULL_RECORDED";
constexpr uint64_t kShadowLabel = 0x40; // obj+0x40: the shadow cull label; obj+0x68: the main one
constexpr uint64_t kMainLabel = 0x68;

// The cull labels XPRPreRender reset and the job has not recorded yet (a few per frame).
std::mutex g_cull_mutex;
std::unordered_set<uint64_t> g_cull_pending;

struct Counters {
    std::atomic<uint64_t> produced{0};     // masks registered
    std::atomic<uint64_t> calls{0};        // consumer masks seen
    std::atomic<uint64_t> served_same{0};  // the copy of this frame's mask (its cull already done)
    std::atomic<uint64_t> served_older{0}; // an earlier frame's mask of the same set entry and slot
    std::atomic<uint64_t> zero_unknown{0}; // the mask was never registered (producer not seen)
    std::atomic<uint64_t> zero_no_copy{0}; // registered, but no complete copy of this key yet
    std::atomic<uint64_t> v_bits{0};       // verify: bits compared
    std::atomic<uint64_t> v_differ{0};     // verify: bits that differ from the real mask
    std::atomic<uint64_t> labels_set{0};   // cull labels stored at record time
    std::atomic<uint64_t> labels_failed{0}; // the store failed (unmapped): the spin then waits for the GPU
};
Counters g_c;

uint8_t* Blocks() {
    static uint8_t* const blocks = []() -> uint8_t* {
        void* p = runtime_low_map ? runtime_low_map(kServeBlocks * kMaxMaskBytes, kProtReadWrite) : nullptr;
        return static_cast<uint8_t*>(p);
    }();
    return blocks;
}

// The mask bytes of an owner's set (4 * ceil(n / 32)), 0 when unreadable.
uint64_t MaskBytes(uint64_t owner) {
    uint64_t set = 0;
    uint16_t count = 0;
    if (!SotcGuest::Read(owner + kOwnerSetOffset, &set) || set == 0 ||
        !SotcGuest::Read(set + kSetCountOffset, &count)) {
        return 0;
    }
    return 4 * ((uint64_t{count} + 31) / 32);
}

// The mask bytes of a decal owner (sub_19c7e0 allocates 4 * ceil(n / 32)): n is the u32 count at its decal object.
uint64_t DecalMaskBytes(uint64_t owner) {
    uint64_t object = 0;
    uint32_t count = 0;
    if (!SotcGuest::Read(owner + kDecalObjectOffset, &object) || object == 0 || !SotcGuest::Read(object, &count)) {
        return 0;
    }
    return 4 * ((uint64_t{count} + 31) / 32);
}

// Registers the mask at guest slot `at` (a guest stack slot holding the pointer) under (owner, slot).
void RegisterMask(uint64_t owner, uint64_t slot, uint64_t at, uint64_t bytes, uint64_t frame) {
    uint64_t mask = 0;
    if (!SotcGuest::Read(at, &mask) || mask == 0) {
        return;
    }
    const uint64_t key = SotcNativeCopies::kKeyMasks | (owner << 6) | slot;
    if (SotcNativeCopies::Register(key, frame, mask, bytes)) {
        g_c.produced.fetch_add(1, std::memory_order_relaxed);
    }
}

// NATIVE_MASK_PRODUCE: site 0x19c0f6 in BPXPR_BuildSetDraws, after the cull dispatch. r15 = the owner;
// slots 0..3 = the four masks of the set entry.
HookAction OnProduce(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    const uint64_t bytes = MaskBytes(regs.r15);
    if (bytes == 0) {
        return HookAction::Continue;
    }
    const uint64_t frame = SotcNativeCopies::CurrentFrame();
    for (uint64_t slot = 0; slot < 4; ++slot) {
        RegisterMask(regs.r15, slot, regs.rsp + kMaskStackOffset + 8 * slot, bytes, frame);
    }
    return HookAction::Continue;
}

// NATIVE_MASK_PRODUCE_SHADOW: site 0x19c6b1 in sub_19c1a0, after the shadow cull dispatch. r12 = the owner;
// slots 4 + 2 * index + {0, 1} (the entry's +0x20 / +0x60 masks of that index).
HookAction OnProduceShadow(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    const uint64_t bytes = MaskBytes(regs.r12);
    uint32_t index = 0;
    if (bytes == 0 || !SotcGuest::Read(regs.rbp + kShadowIndex, &index) || index >= kMaxShadowIndex) {
        return HookAction::Continue;
    }
    const uint64_t frame = SotcNativeCopies::CurrentFrame();
    RegisterMask(regs.r12, 4 + 2 * uint64_t{index}, regs.rbp + kShadowMaskA, bytes, frame);
    RegisterMask(regs.r12, 5 + 2 * uint64_t{index}, regs.rbp + kShadowMaskB, bytes, frame);
    return HookAction::Continue;
}

// NATIVE_MASK_PRODUCE_DECAL: site 0x19ce08 in sub_19c7e0, after the decal cull dispatch (call 0x86b9e0 at 0x19ce03).
// The owner and the mask pointer are read from the frame: the site's displaced instruction is `mov rax,[rbp-0x340]`
// (7 bytes, no RIP operand); [rbp-0x340] and [rbp-0x358] are written once per call, before the dispatch.
HookAction OnProduceDecal(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    uint64_t owner = 0;
    if (!SotcGuest::Read(regs.rbp + kDecalOwnerSlot, &owner) || owner == 0) {
        return HookAction::Continue;
    }
    const uint64_t bytes = DecalMaskBytes(owner);
    if (bytes == 0) {
        return HookAction::Continue;
    }
    RegisterMask(owner, kDecalSlot, regs.rbp + kDecalMaskSlot, bytes, SotcNativeCopies::CurrentFrame());
    return HookAction::Continue;
}

// NATIVE_CULL_RESET: site 0xca9ab in XPRPreRender, after `mov [rax+0x40], 0; mov [rax+0x68], 0`. rax = obj.
HookAction OnCullReset(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    std::lock_guard<std::mutex> lock(g_cull_mutex);
    g_cull_pending.insert(regs.rax + kShadowLabel);
    g_cull_pending.insert(regs.rax + kMainLabel);
    return HookAction::Continue;
}

// NATIVE_DECAL_RESET: site 0xc54c7 in RenderCullXPR (sub_c53d0), `mov qword [rax+0x78], 0` with rax = obj = [cull+0xe8]
// (loaded at 0xc54a2, unchanged up to the site), before the decal cull job is queued. The same pairing as the main
// labels: the record of sub_c5200 (after the decal producers) stores 1, and RenderXPRDecals waits for it.
HookAction OnDecalReset(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    std::lock_guard<std::mutex> lock(g_cull_mutex);
    g_cull_pending.insert(regs.rax + kDecalLabel);
    return HookAction::Continue;
}

// NATIVE_CULL_RECORDED: entry of BPGnm_WriteCsDoneLabel 0x925c0 (rsi = the label). Callers: CullMainScene
// (obj+0x68), the shadow cull (obj+0x40), the decal cull sub_c5200 (obj+0x78, reset by NATIVE_DECAL_RESET).
HookAction OnCullRecorded(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    {
        std::lock_guard<std::mutex> lock(g_cull_mutex);
        if (g_cull_pending.erase(regs.rsi) == 0) {
            return HookAction::Continue;
        }
    }
    // The job stored the mask pointers and draw records before this call (CullMainScene 0xc6f70 and the shadow
    // cull end with it); the consumers may run from here.
    const uint64_t one = 1;
    (SotcGuest::Write(regs.rsi, one) ? g_c.labels_set : g_c.labels_failed).fetch_add(1, std::memory_order_relaxed);
    return HookAction::Continue;
}

uint64_t Take(std::atomic<uint64_t>& c) {
    return c.exchange(0, std::memory_order_relaxed);
}

} // namespace

bool Prev() {
    static const bool prev = [] {
        const char* v = std::getenv("SOTC_NATIVE_MASKS");
        return v != nullptr && std::strcmp(v, "prev") == 0;
    }();
    return prev;
}

void Register() {
    SotcHooks::Handler handler;
    handler.pre = &OnProduce;
    handler.post = nullptr;
    handler.user = nullptr;
    SotcHooks::RegisterHandler(kProduceHook, handler);
    handler.pre = &OnProduceShadow;
    SotcHooks::RegisterHandler(kProduceShadowHook, handler);
    handler.pre = &OnProduceDecal;
    SotcHooks::RegisterHandler(kProduceDecalHook, handler);
    handler.pre = &OnCullReset;
    SotcHooks::RegisterHandler(kCullResetHook, handler);
    handler.pre = &OnDecalReset;
    SotcHooks::RegisterHandler(kDecalResetHook, handler);
    handler.pre = &OnCullRecorded;
    SotcHooks::RegisterHandler(kCullRecordedHook, handler);
}


uint64_t Serve(uint64_t mask, uint64_t count) {
    uint8_t* blocks = Blocks();
    if (!Prev() || !SotcNative::On() || count == 0 || !blocks) {
        return mask;
    }
    g_c.calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t copy_bytes = 4 * ((count + 31) / 32); // what the cull writes
    const uint64_t read_bytes = 8 * ((count + 63) / 64); // what the consumer reads (qwords)
    if (read_bytes > kMaxMaskBytes) {
        return mask;
    }
    uint8_t* block = blocks + (g_next_block.fetch_add(1, std::memory_order_relaxed) % kServeBlocks) * kMaxMaskBytes;
    std::memset(block + copy_bytes, 0, read_bytes - copy_bytes);
    uint64_t key = 0;
    uint64_t seq = 0;
    uint64_t got = 0;
    if (mask == 0 || !SotcNativeCopies::KeyOf(mask, &key, &seq)) { // 0: the entry was not stored yet
        g_c.zero_unknown.fetch_add(1, std::memory_order_relaxed);
    } else {
        got = SotcNativeCopies::CopyOut(key, seq > kOldestFrames ? seq - kOldestFrames : 0, seq, copy_bytes, block);
        if (got == 0) {
            g_c.zero_no_copy.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (got == 0) {
        std::memset(block, 0, copy_bytes);
    }
    if (SotcNative::Verify()) {
        // The real mask is read by the consumer after the spin; its backing holds what the old path read back.
        const unsigned char* real = mask != 0 ? SotcGuest::Backing(mask, copy_bytes) : nullptr;
        if (real != nullptr && got != 0) {
            uint64_t differ = 0;
            for (uint64_t i = 0; i < copy_bytes; ++i) {
                differ += static_cast<uint64_t>(__builtin_popcount(static_cast<unsigned>(real[i] ^ block[i])));
            }
            g_c.v_bits.fetch_add(count, std::memory_order_relaxed);
            g_c.v_differ.fetch_add(differ, std::memory_order_relaxed);
        }
        return mask;
    }
    if (got != 0) {
        (got == seq ? g_c.served_same : g_c.served_older).fetch_add(1, std::memory_order_relaxed);
    }
    return reinterpret_cast<uint64_t>(block);
}

void Report(std::string& out, double seconds) {
    if (!Prev()) {
        return;
    }
    const double secs = seconds > 0.0 ? seconds : 1.0;
    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC NATIVE_MASKS %s: produced %.0f/s, consumer masks %.0f/s, served same %llu older %llu, zero "
                  "masks (never registered %llu, no complete copy %llu); verify bits %llu differ %llu; cull labels "
                  "set at record %llu (failed %llu)\n",
                  SotcNative::Verify() ? "verify" : "prev", static_cast<double>(Take(g_c.produced)) / secs,
                  static_cast<double>(Take(g_c.calls)) / secs, static_cast<unsigned long long>(Take(g_c.served_same)),
                  static_cast<unsigned long long>(Take(g_c.served_older)),
                  static_cast<unsigned long long>(Take(g_c.zero_unknown)),
                  static_cast<unsigned long long>(Take(g_c.zero_no_copy)),
                  static_cast<unsigned long long>(Take(g_c.v_bits)),
                  static_cast<unsigned long long>(Take(g_c.v_differ)),
                  static_cast<unsigned long long>(Take(g_c.labels_set)),
                  static_cast<unsigned long long>(Take(g_c.labels_failed)));
    out += line;
}

} // namespace SotcNativeMasks
