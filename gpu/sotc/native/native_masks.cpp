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
// frame's mask in practice (this frame's cull is not done yet). When no copy of the last kOldestFrames frames is
// complete (the GPU runs behind), the last mask served for the key is served again: a zero mask there drew nothing
// for that frame (1 Nov: grass flicker and objects invisible for a frame, ~800 zero masks per 5 s). A set seen for
// the first time gets a zero mask for that frame (nothing drawn: one frame late, never the all-visible set that
// overflows the command buffer).
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
//
// CPU masks (docs/re-stutters/native/CPUCULL-BRIEF.md, option B). At each producer site the cull block the dispatch
// reads is computed into a CPU mask (cpu_cull.h), stored per key with the frame seq, the object identity and the
// count. Serve returns the CPU mask of the consumer's frame (the seq of the newest registration at the mask) when it
// exists for the same object and count; otherwise the GPU copy path above runs. The cull block of each site:
//   main    the stack copy at [rsp+0x1a0] (0x160 bytes, the memcpy source at 0x19bd5d; rsp is fixed from 0x19b771);
//   shadow  the stack copy at [rbp-0x2e0] (= rsp+0xa0; memcpy source at 0x19c43d, not overwritten before 0x19c6b1);
//   decal   NOT the stack copy (rbp-0x1e0 is reused for a 0x50-byte struct at 0x19ccbc): the heap copy the table
//           object keeps at [table+0xb0] (stored at 0x19cc03; the 0x20-byte struct built for sub_ae150 at 0x19cdd4
//           points at it), where table = [rbp-0x360] (r13 at the site, the second argument of sub_19c7e0).
// The two masks of a shadow index and the four masks of a main set are computed and stored together, so a served
// pair (the VIS_IND consumer ANDs two masks) comes from one frame, or falls back as one.
//
// Verify (SOTC_NATIVE=2): a served CPU mask is compared bit by bit with the real GPU mask of the same frame once its
// GPU copy is complete (deferred: the copy of a frame completes after the consumer ran). Missing bits (real set, CPU
// clear) must stay 0; extra bits are the objects the conservative CPU mask keeps that the GPU clears.
#include "native_masks.h"
#include "verify.h" // sotc: SOTC_VERIFY (the CPU mask comparison in mode 1)
#include "../cmd_counters.h" // sotc: SOTC_CMD_COUNTERS (zero-mask counts)

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../guest_mem.h"
#include "../hooks.h"
#include "cpu_cull.h"
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
constexpr int64_t kDecalTableSlot = -0x360;      // [rbp-0x360]: the second argument (r13 at the site), holds the cull block
constexpr uint64_t kDecalCullBlockSlot = 0xb0;   // table+0xb0: the heap copy of the 0x160-byte cull block the GPU reads
constexpr uint64_t kDecalSlot = 32;              // the key slot of the decal mask (main 0..3, shadow 4..19)
constexpr uint64_t kDecalObjectOffset = 0x20;    // owner+0x20: the decal object; its u32 count at +0
constexpr uint64_t kDecalLabel = 0x78;           // obj+0x78, obj = [cull+0xe8] (rax at the reset site)
constexpr int64_t kShadowMaskA = -0x368;         // [rbp-0x368], [rbp-0x360]: the two shadow masks at the site
constexpr int64_t kShadowMaskB = -0x360;
constexpr int64_t kShadowIndex = -0x348;         // int: the shadow slot index of the call
constexpr int64_t kShadowCullBlock = -0x2e0;     // rbp-0x2e0: the 0x160-byte cull block of sub_19c1a0 (rsp+0xa0)
constexpr uint64_t kMaxShadowIndex = 8;
constexpr uint64_t kMainCullBlock = 0x1a0;       // rsp+0x1a0: the 0x160-byte cull block of BuildSetDraws
constexpr int kProtReadWrite = 1 | 2;            // PROT_READ | PROT_WRITE (src/compat_win.h)
constexpr uint64_t kOwnerSetOffset = 0x18;       // owner+0x18: the set record (its u16 count at +0x64)
constexpr uint64_t kSetCountOffset = 0x64;
constexpr uint64_t kMaskStackOffset = 0x140;     // [rsp+0x140..+0x158]: the four mask pointers at the site
constexpr uint64_t kMaxMaskBytes = 8 * 1024;     // 65535 bits rounded up to qwords
constexpr uint64_t kServeBlocks = 64;            // consumers read their block during the call only
constexpr uint64_t kOldestFrames = 3;            // serve this frame .. three frames back
// Shadow slots (kShadowIndex): 4 + 2 * index + {0, 1}, index 0..7 (the cascades 0..3 and the scene loop 4..).
constexpr uint64_t kShadowSlotFirst = 4;
constexpr uint64_t kShadowSlotLast = 19;

// The verify classes of the CPU masks: the four main slots, the shadow slots and the decal mask.
enum CullClass : int { kClassMain0 = 0, kClassShadow = 4, kClassDecal = 5, kClasses = 6 };
constexpr const char* kClassName[kClasses] = {"main0", "main1", "main2", "main3", "shadow", "decal"};

int ClassOf(uint64_t slot) {
    if (slot < 4) {
        return static_cast<int>(slot);
    }
    if (slot >= kShadowSlotFirst && slot <= kShadowSlotLast) {
        return kClassShadow;
    }
    return kClassDecal;
}

std::atomic<uint64_t> g_next_block{0};

// The last mask served per key, for a frame without a complete copy. The table holds at most kMaxLastMasks keys:
// when it is full the keys with no live registration are evicted, and the table is cleared only if that is not
// enough (a cleared key falls back to the zero mask for one frame).
constexpr size_t kMaxLastMasks = 16384;
struct LastMask {
    uint64_t identity;          // the object behind the key when the copy was served
    std::vector<uint8_t> bytes;
};
std::mutex g_last_mutex;
std::unordered_map<uint64_t, LastMask> g_last_masks;

constexpr const char* kCullResetHook = "NATIVE_CULL_RESET";
constexpr const char* kCullRecordedHook = "NATIVE_CULL_RECORDED";
constexpr uint64_t kShadowLabel = 0x40; // obj+0x40: the shadow cull label; obj+0x68: the main one
constexpr uint64_t kMainLabel = 0x68;

// The cull labels XPRPreRender reset and the job has not recorded yet (a few per frame).
std::mutex g_cull_mutex;
std::unordered_set<uint64_t> g_cull_pending;

// CPU masks per key (CPUCULL-BRIEF). A key keeps the last kCpuRing frames: the consumer of a frame reads the
// newest registration of its key, which is the mask of its own frame.
constexpr uint64_t kCpuRing = 3;
constexpr size_t kMaxCpuKeys = 16384;
struct CpuMask {
    uint64_t seq = 0;           // the frame of the registration
    uint64_t identity = 0;      // the set record (main, shadow) or the decal object
    uint64_t count = 0;         // the object count of the set
    bool valid = false;
    std::vector<uint32_t> bits; // (count + 31) / 32 dwords, the bits past count clear
};
struct CpuKey {
    CpuMask ring[kCpuRing];
};
std::mutex g_cpu_mutex;
std::unordered_map<uint64_t, CpuKey> g_cpu;

// Deferred verify: the served CPU masks wait until the GPU copy of their frame is complete.
constexpr size_t kMaxPending = 16384;
constexpr size_t kDrainPerCall = 32;
struct PendingVerify {
    uint64_t key = 0;
    uint64_t seq = 0;
    uint64_t count = 0;
    int cls = 0;
    std::vector<uint32_t> cpu;
};
std::mutex g_verify_mutex;
std::deque<PendingVerify> g_pending;

struct Counters {
    std::atomic<uint64_t> produced{0};     // masks registered
    std::atomic<uint64_t> calls{0};        // consumer masks seen
    std::atomic<uint64_t> served_same{0};  // the copy of this frame's mask (its cull already done)
    std::atomic<uint64_t> served_older{0}; // an earlier frame's mask of the same set entry and slot
    std::atomic<uint64_t> zero_unknown{0}; // the mask was never registered (producer not seen)
    std::atomic<uint64_t> zero_no_copy{0}; // registered, but no complete copy of this key yet
    std::atomic<uint64_t> served_last{0};  // no complete copy: the last mask served for the key again
    std::atomic<uint64_t> shadow_or{0};    // shadow slot: served OR of the newest copy and the one before it
    std::atomic<uint64_t> v_bits{0};       // verify: bits compared
    std::atomic<uint64_t> v_differ{0};     // verify: bits that differ from the real mask
    std::atomic<uint64_t> labels_set{0};   // cull labels stored at record time
    std::atomic<uint64_t> labels_failed{0}; // the store failed (unmapped): the spin then waits for the GPU
    std::atomic<uint64_t> zero_reason[SotcNativeCopies::kMissKinds]{}; // zero_no_copy by CopyOut's reason
    std::atomic<uint64_t> last_evicted{0};  // last-served keys dropped because their registrations are gone
    std::atomic<uint64_t> last_cleared{0};  // the whole last-served table dropped (still full after eviction)
    // CPU masks (CPUCULL-BRIEF).
    std::atomic<uint64_t> cpu_sets{0};      // producer sets whose CPU masks were computed
    std::atomic<uint64_t> cpu_failed{0};    // producer sets without CPU masks (CullX false, input unreadable)
    std::atomic<uint64_t> cpu_ns{0};        // time spent in the CPU cull at the producer sites
    std::atomic<uint64_t> cpu_cleared{0};   // the CPU mask table cleared (still full)
    std::atomic<uint64_t> served_cpu{0};    // consumer calls served the CPU mask of their frame
    std::atomic<uint64_t> fallback{0};      // consumer calls served by the GPU copy path (or a zero mask)
    std::atomic<uint64_t> v_pending_dropped{0}; // deferred verify: queue full, the oldest entry dropped
    std::atomic<uint64_t> v_unverified{0};  // deferred verify: the GPU copy never completed or was overwritten
    std::atomic<uint64_t> v_masks[kClasses]{};   // verify, per class: masks compared
    std::atomic<uint64_t> v_cls_bits[kClasses]{}; // verify, per class: bits below the count
    std::atomic<uint64_t> v_real[kClasses]{};     // verify, per class: bits set in the real mask
    std::atomic<uint64_t> v_cpu[kClasses]{};      // verify, per class: bits set in the CPU mask
    std::atomic<uint64_t> v_missing[kClasses]{};  // verify, per class: real set, CPU clear (must be 0)
    std::atomic<uint64_t> v_extra[kClasses]{};    // verify, per class: CPU set, real clear (no Hi-Z cost)
};
Counters g_c;

uint8_t* Blocks() {
    static uint8_t* const blocks = []() -> uint8_t* {
        void* p = runtime_low_map ? runtime_low_map(kServeBlocks * kMaxMaskBytes, kProtReadWrite) : nullptr;
        return static_cast<uint8_t*>(p);
    }();
    return blocks;
}

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The key of the mask of (owner, slot): the same key the copy ring and the consumers' KeyOf use.
uint64_t MaskKey(uint64_t owner, uint64_t slot) {
    return SotcNativeCopies::kKeyMasks | (owner << 6) | slot;
}

// Clears the bits past `count` in the last dword (the consumer reads whole qwords; those bits are no object).
void TailClear(uint32_t* bits, uint64_t count) {
    const uint64_t dwords = (count + 31) / 32;
    if (dwords != 0 && count % 32 != 0) {
        bits[dwords - 1] &= (uint32_t{1} << (count % 32)) - 1;
    }
}

// Thread-local scratch for the CPU masks of one producer call. Grown once; no allocation per call.
uint32_t* Scratch(uint64_t dwords) {
    thread_local std::vector<uint32_t> buf;
    if (buf.size() < dwords) {
        buf.resize(dwords);
    }
    return buf.data();
}

// The mask bytes of an owner's set (4 * ceil(n / 32)), 0 when unreadable. `set_out` gets the set record (the
// object identity of the masks: a new set behind the same owner is a new record), `count_out` its object count.
uint64_t MaskBytes(uint64_t owner, uint64_t* set_out, uint64_t* count_out) {
    uint64_t set = 0;
    uint16_t count = 0;
    if (!SotcGuest::Read(owner + kOwnerSetOffset, &set) || set == 0 ||
        !SotcGuest::Read(set + kSetCountOffset, &count)) {
        return 0;
    }
    *set_out = set;
    *count_out = count;
    return 4 * ((uint64_t{count} + 31) / 32);
}

// The mask bytes of a decal owner (sub_19c7e0 allocates 4 * ceil(n / 32)): n is the u32 count at its decal object,
// which is also the object identity.
uint64_t DecalMaskBytes(uint64_t owner, uint64_t* object_out, uint64_t* count_out) {
    uint64_t object = 0;
    uint32_t count = 0;
    if (!SotcGuest::Read(owner + kDecalObjectOffset, &object) || object == 0 || !SotcGuest::Read(object, &count)) {
        return 0;
    }
    *object_out = object;
    *count_out = count;
    return 4 * ((uint64_t{count} + 31) / 32);
}

// Registers the mask at guest slot `at` (a guest stack slot holding the pointer) under (owner, slot). Each mask
// takes exactly one cull write (single write: paired in order per base, see native_copies.cpp).
void RegisterMask(uint64_t owner, uint64_t slot, uint64_t at, uint64_t bytes, uint64_t frame, uint64_t identity) {
    uint64_t mask = 0;
    if (!SotcGuest::Read(at, &mask) || mask == 0) {
        return;
    }
    if (SotcNativeCopies::Register(MaskKey(owner, slot), frame, mask, bytes, identity, true)) {
        g_c.produced.fetch_add(1, std::memory_order_relaxed);
    }
}

// Stores the CPU mask of (key, seq). Replaces the entry of the same frame; otherwise takes a free entry or the
// oldest one. Caller holds nothing. The bits are copied (the vectors keep their capacity).
void StoreCpu(uint64_t key, uint64_t seq, uint64_t identity, uint64_t count, const uint32_t* bits) {
    const uint64_t dwords = (count + 31) / 32;
    std::lock_guard<std::mutex> lock(g_cpu_mutex);
    if (g_cpu.size() >= kMaxCpuKeys && g_cpu.find(key) == g_cpu.end()) {
        g_cpu.clear();
        g_c.cpu_cleared.fetch_add(1, std::memory_order_relaxed);
    }
    CpuKey& k = g_cpu[key];
    CpuMask* slot = nullptr;
    for (CpuMask& m : k.ring) {
        if (m.valid && m.seq == seq) {
            slot = &m;
            break;
        }
    }
    if (slot == nullptr) {
        for (CpuMask& m : k.ring) {
            if (!m.valid) {
                slot = &m;
                break;
            }
        }
    }
    if (slot == nullptr) {
        slot = &k.ring[0];
        for (CpuMask& m : k.ring) {
            if (m.seq < slot->seq) {
                slot = &m;
            }
        }
    }
    slot->seq = seq;
    slot->identity = identity;
    slot->count = count;
    slot->valid = true;
    slot->bits.assign(bits, bits + dwords);
}

// Copies the CPU mask of (key, seq) into dst (4 * ceil(count / 32) bytes) when the table holds it for the same
// object and the same count. False otherwise: the caller takes the GPU copy path.
bool FindCpu(uint64_t key, uint64_t seq, uint64_t identity, uint64_t count, uint8_t* dst) {
    std::lock_guard<std::mutex> lock(g_cpu_mutex);
    const auto it = g_cpu.find(key);
    if (it == g_cpu.end()) {
        return false;
    }
    for (const CpuMask& m : it->second.ring) {
        if (m.valid && m.seq == seq && m.identity == identity && m.count == count) {
            std::memcpy(dst, m.bits.data(), m.bits.size() * sizeof(uint32_t));
            return true;
        }
    }
    return false;
}

// The CPU cull of a main set (BPXPR_BuildSetDraws, site 0x19c0f6): the four masks, stored under slots 0..3.
void ProduceMain(uint64_t owner, uint64_t set, uint64_t count, uint64_t cull_block, uint64_t frame) {
    if (!Cpu()) {
        return;
    }
    const uint64_t t0 = NowNs();
    const uint64_t dwords = (count + 31) / 32;
    uint32_t* base = Scratch(4 * dwords);
    uint32_t* masks[4] = {base, base + dwords, base + 2 * dwords, base + 3 * dwords};
    if (SotcCpuCull::CullMain(owner, cull_block, static_cast<uint32_t>(count), masks)) {
        for (uint64_t slot = 0; slot < 4; ++slot) {
            TailClear(masks[slot], count);
            StoreCpu(MaskKey(owner, slot), frame, set, count, masks[slot]);
        }
        g_c.cpu_sets.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_c.cpu_failed.fetch_add(1, std::memory_order_relaxed);
    }
    g_c.cpu_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    SotcCpuCull::AddStageNs(SotcCpuCull::Stage::kMain, NowNs() - t0);
}

// The CPU cull of one shadow index (sub_19c1a0, site 0x19c6b1): two masks, slots 4 + 2 * index + {0, 1}.
void ProduceShadow(uint64_t owner, uint64_t set, uint64_t count, uint64_t cull_block, uint32_t index,
                   uint64_t frame) {
    if (!Cpu()) {
        return;
    }
    const uint64_t t0 = NowNs();
    const uint64_t dwords = (count + 31) / 32;
    uint32_t* base = Scratch(2 * dwords);
    uint32_t* masks[2] = {base, base + dwords};
    if (SotcCpuCull::CullShadow(owner, cull_block, index, static_cast<uint32_t>(count), masks)) {
        for (uint64_t i = 0; i < 2; ++i) {
            TailClear(masks[i], count);
            StoreCpu(MaskKey(owner, 4 + 2 * uint64_t{index} + i), frame, set, count, masks[i]);
        }
        g_c.cpu_sets.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_c.cpu_failed.fetch_add(1, std::memory_order_relaxed);
    }
    g_c.cpu_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    SotcCpuCull::AddStageNs(SotcCpuCull::Stage::kShadow, NowNs() - t0);
}

// The CPU cull of one decal owner (sub_19c7e0, site 0x19ce08): one mask, slot kDecalSlot, identity = the object.
void ProduceDecal(uint64_t owner, uint64_t object, uint64_t count, uint64_t cull_block, uint64_t frame) {
    if (!Cpu()) {
        return;
    }
    const uint64_t t0 = NowNs();
    const uint64_t dwords = (count + 31) / 32;
    uint32_t* mask = Scratch(dwords);
    if (SotcCpuCull::CullDecal(owner, cull_block, static_cast<uint32_t>(count), mask)) {
        TailClear(mask, count);
        StoreCpu(MaskKey(owner, kDecalSlot), frame, object, count, mask);
        g_c.cpu_sets.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_c.cpu_failed.fetch_add(1, std::memory_order_relaxed);
    }
    g_c.cpu_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
}

// NATIVE_MASK_PRODUCE: site 0x19c0f6 in BPXPR_BuildSetDraws, after the cull dispatch. r15 = the owner;
// slots 0..3 = the four masks of the set entry. The cull block is the stack copy at [rsp+0x1a0].
HookAction OnProduce(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    uint64_t set = 0;
    uint64_t count = 0;
    const uint64_t bytes = MaskBytes(regs.r15, &set, &count);
    if (bytes == 0) {
        return HookAction::Continue;
    }
    const uint64_t frame = SotcNativeCopies::CurrentFrame();
    for (uint64_t slot = 0; slot < 4; ++slot) {
        RegisterMask(regs.r15, slot, regs.rsp + kMaskStackOffset + 8 * slot, bytes, frame, set);
    }
    ProduceMain(regs.r15, set, count, regs.rsp + kMainCullBlock, frame);
    return HookAction::Continue;
}

// NATIVE_MASK_PRODUCE_SHADOW: site 0x19c6b1 in sub_19c1a0, after the shadow cull dispatch. r12 = the owner;
// slots 4 + 2 * index + {0, 1} (the entry's +0x20 / +0x60 masks of that index). The cull block is the stack copy
// at [rbp-0x2e0].
HookAction OnProduceShadow(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    uint64_t set = 0;
    uint64_t count = 0;
    const uint64_t bytes = MaskBytes(regs.r12, &set, &count);
    uint32_t index = 0;
    if (bytes == 0 || !SotcGuest::Read(regs.rbp + kShadowIndex, &index) || index >= kMaxShadowIndex) {
        return HookAction::Continue;
    }
    const uint64_t frame = SotcNativeCopies::CurrentFrame();
    RegisterMask(regs.r12, 4 + 2 * uint64_t{index}, regs.rbp + kShadowMaskA, bytes, frame, set);
    RegisterMask(regs.r12, 5 + 2 * uint64_t{index}, regs.rbp + kShadowMaskB, bytes, frame, set);
    ProduceShadow(regs.r12, set, count, regs.rbp + kShadowCullBlock, index, frame);
    return HookAction::Continue;
}

// NATIVE_MASK_PRODUCE_DECAL: site 0x19ce08 in sub_19c7e0, after the decal cull dispatch (call 0x86b9e0 at 0x19ce03).
// The owner and the mask pointer are read from the frame: the site's displaced instruction is `mov rax,[rbp-0x340]`
// (7 bytes, no RIP operand); [rbp-0x340] and [rbp-0x358] are written once per call, before the dispatch.
// The cull block is not on the stack at the site: it is the heap copy at table+0xb0, table = [rbp-0x360].
HookAction OnProduceDecal(GuestRegs& regs, void*) {
    if (!Prev() || !SotcNative::On()) {
        return HookAction::Continue;
    }
    uint64_t owner = 0;
    if (!SotcGuest::Read(regs.rbp + kDecalOwnerSlot, &owner) || owner == 0) {
        return HookAction::Continue;
    }
    uint64_t object = 0;
    uint64_t count = 0;
    const uint64_t bytes = DecalMaskBytes(owner, &object, &count);
    if (bytes == 0) {
        return HookAction::Continue;
    }
    const uint64_t frame = SotcNativeCopies::CurrentFrame();
    RegisterMask(owner, kDecalSlot, regs.rbp + kDecalMaskSlot, bytes, frame, object);
    uint64_t table = 0; // the decal table object of sub_19c7e0's second argument (not the set table entry)
    uint64_t cull_block = 0;
    if (SotcGuest::Read(regs.rbp + kDecalTableSlot, &table) && table != 0 &&
        SotcGuest::Read(table + kDecalCullBlockSlot, &cull_block) && cull_block != 0) {
        ProduceDecal(owner, object, count, cull_block, frame);
    } else {
        g_c.cpu_failed.fetch_add(1, std::memory_order_relaxed);
    }
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

// Caller holds g_last_mutex. Keys without a live registration go first; the table is cleared only when still full.
void EvictLastLocked() {
    for (auto it = g_last_masks.begin(); it != g_last_masks.end();) {
        if (!SotcNativeCopies::KeyLive(it->first)) {
            it = g_last_masks.erase(it);
            g_c.last_evicted.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
    if (g_last_masks.size() >= kMaxLastMasks) {
        g_c.last_cleared.fetch_add(1, std::memory_order_relaxed);
        g_last_masks.clear();
    }
}

// Verify: a served CPU mask waits for the GPU copy of its frame (DrainVerify compares it).
void EnqueueVerify(uint64_t key, uint64_t seq, uint64_t count, uint64_t copy_bytes, const uint8_t* cpu) {
    PendingVerify p;
    p.key = key;
    p.seq = seq;
    p.count = count;
    p.cls = ClassOf(key & 63);
    p.cpu.resize(copy_bytes / 4);
    std::memcpy(p.cpu.data(), cpu, copy_bytes);
    std::lock_guard<std::mutex> lock(g_verify_mutex);
    if (g_pending.size() >= kMaxPending) {
        g_pending.pop_front();
        g_c.v_pending_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_pending.push_back(std::move(p));
}

// Verify: compares one CPU mask with the real mask of its frame, bit by bit, for the bits below the count.
void CompareVerify(const PendingVerify& p, const uint32_t* real, uint64_t dwords) {
    uint64_t real_bits = 0;
    uint64_t cpu_bits = 0;
    uint64_t missing = 0;
    uint64_t extra = 0;
    for (uint64_t w = 0; w < dwords; ++w) {
        uint32_t valid = ~uint32_t{0};
        if (w == dwords - 1 && p.count % 32 != 0) {
            valid = (uint32_t{1} << (p.count % 32)) - 1;
        }
        const uint32_t r = real[w] & valid;
        const uint32_t c = p.cpu[w] & valid;
        real_bits += static_cast<uint64_t>(std::popcount(r));
        cpu_bits += static_cast<uint64_t>(std::popcount(c));
        missing += static_cast<uint64_t>(std::popcount(r & ~c));
        extra += static_cast<uint64_t>(std::popcount(c & ~r));
    }
    const int k = p.cls;
    g_c.v_masks[k].fetch_add(1, std::memory_order_relaxed);
    g_c.v_cls_bits[k].fetch_add(p.count, std::memory_order_relaxed);
    g_c.v_real[k].fetch_add(real_bits, std::memory_order_relaxed);
    g_c.v_cpu[k].fetch_add(cpu_bits, std::memory_order_relaxed);
    g_c.v_missing[k].fetch_add(missing, std::memory_order_relaxed);
    g_c.v_extra[k].fetch_add(extra, std::memory_order_relaxed);
}

// Verify: compares the queued CPU masks whose GPU copy is complete, oldest first, at most `limit` per call. A copy
// that is not complete yet (the frame or its async tick is still running) stops the drain; any other miss drops it.
void DrainVerify(size_t limit) {
    std::lock_guard<std::mutex> lock(g_verify_mutex);
    thread_local std::vector<uint32_t> real;
    for (size_t n = 0; n < limit && !g_pending.empty(); ++n) {
        const PendingVerify& p = g_pending.front();
        const uint64_t dwords = p.cpu.size();
        real.resize(dwords);
        SotcNativeCopies::Miss miss = SotcNativeCopies::Miss::kNoKey;
        const uint64_t got = SotcNativeCopies::CopyOut(p.key, p.seq, p.seq, dwords * 4, real.data(), nullptr, &miss);
        if (got != p.seq) {
            if (miss == SotcNativeCopies::Miss::kFrame || miss == SotcNativeCopies::Miss::kAsync) {
                break;
            }
            g_c.v_unverified.fetch_add(1, std::memory_order_relaxed);
        } else {
            CompareVerify(p, real.data(), dwords);
        }
        g_pending.pop_front();
    }
}

} // namespace

bool Prev() {
    static const bool prev = [] {
        const char* v = std::getenv("SOTC_NATIVE_MASKS");
        return v != nullptr && (std::strcmp(v, "prev") == 0 || std::strcmp(v, "cpu") == 0);
    }();
    return prev;
}

bool Cpu() {
    static const bool cpu = [] {
        const char* v = std::getenv("SOTC_NATIVE_MASKS");
        return v != nullptr && std::strcmp(v, "cpu") == 0;
    }();
    return cpu;
}

void Register() {
    SotcCpuCull::RegisterLod(); // sotc: NATIVE_LOD_DISPATCH, the CPU mirror of the LOD pre-shader (cpu_cull_lod.cpp)
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
    const uint64_t t_serve = NowNs();
    const uint64_t copy_bytes = 4 * ((count + 31) / 32); // what the cull writes
    const uint64_t read_bytes = 8 * ((count + 63) / 64); // what the consumer reads (qwords)
    if (read_bytes > kMaxMaskBytes) {
        return mask;
    }
    uint8_t* block = blocks + (g_next_block.fetch_add(1, std::memory_order_relaxed) % kServeBlocks) * kMaxMaskBytes;
    std::memset(block + copy_bytes, 0, read_bytes - copy_bytes);
    uint64_t key = 0;
    uint64_t seq = 0;
    uint64_t identity = 0;
    const bool known = mask != 0 && SotcNativeCopies::KeyOf(mask, &key, &seq, &identity); // 0: not stored yet
    // sotc: the CPU mask of this frame (CPUCULL-BRIEF). seq is the frame of the newest registration at mask, so this
    // is the mask of the consumer's own frame. Served without any GPU copy; the shadow OR is not needed then.
    if (known && FindCpu(key, seq, identity, count, block)) {
        g_c.served_cpu.fetch_add(1, std::memory_order_relaxed);
        SotcCpuCull::AddStageNs(SotcCpuCull::Stage::kServe, NowNs() - t_serve);
        if (SotcVerify::VerifyAll() || SotcNative::Verify()) { // SOTC_VERIFY: compare with the GPU copy
            const uint64_t t_verify = NowNs();
            EnqueueVerify(key, seq, count, copy_bytes, block);
            DrainVerify(kDrainPerCall);
            SotcCpuCull::AddStageNs(SotcCpuCull::Stage::kVerify, NowNs() - t_verify);
        }
        if (SotcNative::Verify()) {
            return mask;
        }
        return reinterpret_cast<uint64_t>(block);
    }
    // Fallback: the newest complete GPU copy (the path before the CPU masks).
    g_c.fallback.fetch_add(1, std::memory_order_relaxed);
    uint64_t got = 0;
    bool last = false;
    SotcNativeCopies::Miss miss = SotcNativeCopies::Miss::kNoKey;
    if (!known) {
        g_c.zero_unknown.fetch_add(1, std::memory_order_relaxed);
    } else {
        got = SotcNativeCopies::CopyOut(key, seq > kOldestFrames ? seq - kOldestFrames : 0, seq, copy_bytes, block,
                                        nullptr, &miss);
        const uint64_t slot = key & 63;
        if (got != 0 && slot >= kShadowSlotFirst && slot <= kShadowSlotLast) {
            // sotc: a shadow cascade is fitted per frame, so the copy of one frame may drop casters the
            // cascade of this frame sees (and a parked camera flickers when the fit alternates). The
            // served mask is the OR of the newest copy and the complete copy before it: a caster that
            // was in either frame is drawn. No GPU wait: both copies are complete already.
            static thread_local std::vector<uint8_t> older;
            older.resize(copy_bytes);
            if (SotcNativeCopies::CopyOut(key, got > kOldestFrames ? got - kOldestFrames : 0, got - 1, copy_bytes,
                                          older.data()) != 0) {
                for (uint64_t i = 0; i < copy_bytes; ++i) {
                    block[i] |= older[i];
                }
                g_c.shadow_or.fetch_add(1, std::memory_order_relaxed);
            }
        }
        std::lock_guard<std::mutex> lock(g_last_mutex);
        if (got != 0) {
            if (g_last_masks.size() >= kMaxLastMasks) {
                EvictLastLocked();
            }
            LastMask& l = g_last_masks[key];
            l.identity = identity;
            l.bytes.assign(block, block + copy_bytes);
        } else if (const auto it = g_last_masks.find(key);
                   it != g_last_masks.end() && it->second.bytes.size() == copy_bytes &&
                   it->second.identity == identity) {
            std::memcpy(block, it->second.bytes.data(), copy_bytes);
            g_c.served_last.fetch_add(1, std::memory_order_relaxed);
            last = true;
        } else {
            g_c.zero_no_copy.fetch_add(1, std::memory_order_relaxed);
            g_c.zero_reason[static_cast<int>(miss)].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (got == 0 && !last) {
        if (SotcCmdCounters::kOn) { // sotc: SOTC_CMD_COUNTERS, zero masks per key (count only)
            SotcCmdCounters::NoteZeroMask(known ? key : 0, !known);
        }
        std::memset(block, 0, copy_bytes);
    }
    if (SotcNative::Verify()) {
        // The real mask is read by the consumer after the spin; its backing holds what the old path read back.
        const unsigned char* real = known ? SotcGuest::Backing(mask, copy_bytes) : nullptr;
        if (real != nullptr && got != 0) {
            uint64_t differ = 0;
            for (uint64_t i = 0; i < copy_bytes; ++i) {
                differ += static_cast<uint64_t>(__builtin_popcount(static_cast<unsigned>(real[i] ^ block[i])));
            }
            g_c.v_bits.fetch_add(count, std::memory_order_relaxed);
            g_c.v_differ.fetch_add(differ, std::memory_order_relaxed);
        }
        DrainVerify(kDrainPerCall);
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
    SotcNativeCopies::Stats st;
    SotcNativeCopies::TakeStats(st);
    const auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    const auto reason = [](SotcNativeCopies::Miss m) {
        return static_cast<unsigned long long>(Take(g_c.zero_reason[static_cast<int>(m)]));
    };
    char line[1400];
    std::snprintf(
        line, sizeof line,
        "SOTC NATIVE_MASKS %s: produced %.0f/s, consumer masks %.0f/s, served same %llu older %llu last %llu shadow or %llu, "
        "zero masks (never registered %llu, no copy %llu: nokey %llu, ring %llu, frame %llu, nowrite %llu, "
        "short %llu, uncovered %llu, async %llu, staging %llu), verify bits %llu differ %llu; cull labels set at "
        "record %llu (failed %llu); writes paired %llu, unpaired %llu, expired %llu, ring pruned %llu (live %llu), "
        "count pruned %llu, same-frame kept %llu, identity changed %llu, forgot %llu, key missing %llu, key invalid "
        "%llu; lag max %llu frames, write lag max %llu frames, ring span max %.0f KiB; last evicted %llu, cleared "
        "%llu\n",
        SotcNative::Verify() ? "verify" : "prev", static_cast<double>(Take(g_c.produced)) / secs,
        static_cast<double>(Take(g_c.calls)) / secs, ull(Take(g_c.served_same)), ull(Take(g_c.served_older)),
        ull(Take(g_c.served_last)), ull(Take(g_c.shadow_or)), ull(Take(g_c.zero_unknown)), ull(Take(g_c.zero_no_copy)),
        reason(SotcNativeCopies::Miss::kNoKey), reason(SotcNativeCopies::Miss::kInvalid),
        reason(SotcNativeCopies::Miss::kFrame), reason(SotcNativeCopies::Miss::kNoWrite),
        reason(SotcNativeCopies::Miss::kShort), reason(SotcNativeCopies::Miss::kNotCovered),
        reason(SotcNativeCopies::Miss::kAsync), reason(SotcNativeCopies::Miss::kNoStaging),
        ull(Take(g_c.v_bits)), ull(Take(g_c.v_differ)), ull(Take(g_c.labels_set)), ull(Take(g_c.labels_failed)),
        ull(st.paired), ull(st.unpaired), ull(st.expired), ull(st.pruned_ring), ull(st.pruned_ring_live),
        ull(st.pruned_count), ull(st.kept_same_frame), ull(st.identity_changed), ull(st.forgot), ull(st.key_missing),
        ull(st.key_invalid), ull(st.lag_max), ull(st.write_lag_max), static_cast<double>(st.ring_span_max) / 1024.0,
        ull(Take(g_c.last_evicted)), ull(Take(g_c.last_cleared)));
    out += line;

    // sotc: CPU masks (CPUCULL-BRIEF): the producer cost per second, the served masks and the fallbacks.
    const double cpu_ms_per_s = static_cast<double>(Take(g_c.cpu_ns)) / 1e6 / secs;
    // sotc: the LOD mirror (cpu_cull_lod.cpp): verify compares it with the GPU tables of the completed frames first.
    SotcCpuCull::DrainLodVerify(static_cast<size_t>(-1));
    SotcCpuCull::LodStats lod{};
    SotcCpuCull::TakeLodStats(lod);
    const auto stage_ms = [&](SotcCpuCull::Stage s) {
        return static_cast<double>(SotcCpuCull::TakeStageNs(s)) / 1e6 / secs;
    };
    const double st_lod = stage_ms(SotcCpuCull::Stage::kLodStep);
    const double st_enq = stage_ms(SotcCpuCull::Stage::kLodEnqueue);
    const double st_worker = stage_ms(SotcCpuCull::Stage::kLodWorker);
    const double st_main = stage_ms(SotcCpuCull::Stage::kMain);
    const double st_shadow = stage_ms(SotcCpuCull::Stage::kShadow);
    const double st_serve = stage_ms(SotcCpuCull::Stage::kServe);
    const double st_verify = stage_ms(SotcCpuCull::Stage::kVerify);
    char cpu_line[1100];
    std::snprintf(cpu_line, sizeof cpu_line,
                  "SOTC NATIVE_MASKS cpu %s: sets %.0f/s (failed %llu), cpu time %.3f ms/s, served cpu %.0f/s, "
                  "fallback %.0f/s, table cleared %llu, lod dispatches %llu (models %llu, failed %llu, sampled %llu), "
                  "lod verified %llu models (unverified %llu), lod_mismatch %llu, fade_mismatch %llu; "
                  "game-thread ms/s: lod step %.3f, lod enqueue %.3f, cull main %.3f, cull shadow %.3f, serve %.3f, "
                  "mask verify %.3f (total %.3f); worker thread ms/s: lod verify %.3f\n",
                  SotcNative::Verify() ? "verify" : "prev", static_cast<double>(Take(g_c.cpu_sets)) / secs,
                  ull(Take(g_c.cpu_failed)), cpu_ms_per_s, static_cast<double>(Take(g_c.served_cpu)) / secs,
                  static_cast<double>(Take(g_c.fallback)) / secs, ull(Take(g_c.cpu_cleared)), ull(lod.dispatches),
                  ull(lod.models), ull(lod.failed), ull(lod.sampled), ull(lod.verified), ull(lod.unverified),
                  ull(lod.lod_mismatch), ull(lod.fade_mismatch), st_lod, st_enq, st_main, st_shadow, st_serve,
                  st_verify, st_lod + st_enq + st_main + st_shadow + st_serve + st_verify, st_worker);
    out += cpu_line;

    if (!SotcNative::Verify() && !SotcVerify::VerifyAll()) {
        return;
    }
    DrainVerify(static_cast<size_t>(-1));
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(g_verify_mutex);
        pending = g_pending.size();
    }
    char head[300];
    std::snprintf(head, sizeof head,
                  "SOTC NATIVE_MASKS verify cpu: pending %zu, dropped %llu, unverified %llu; per class (missing must be 0):",
                  pending, ull(Take(g_c.v_pending_dropped)), ull(Take(g_c.v_unverified)));
    std::string verify = head;
    for (int k = 0; k < kClasses; ++k) {
        char part[256];
        std::snprintf(part, sizeof part,
                      " %s masks %llu bits %llu real %llu cpu %llu missing %llu extra %llu;", kClassName[k],
                      ull(Take(g_c.v_masks[k])), ull(Take(g_c.v_cls_bits[k])), ull(Take(g_c.v_real[k])),
                      ull(Take(g_c.v_cpu[k])), ull(Take(g_c.v_missing[k])), ull(Take(g_c.v_extra[k])));
        verify += part;
    }
    out += verify + "\n";
}

} // namespace SotcNativeMasks
