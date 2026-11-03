// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native GPU-result consumers (docs/re-stutters/native/impl-results.md). Hooks in
// scripts/hooks.d/native_results.py, byte patches in scripts/sotc_modes.py (native_results_patches).
//
// Masks (SOTC_NATIVE_MASKS=all): the four XPR visibility-mask consumers draw the objects whose bit is
// set in a mask that the GPU writes. Their entry hooks hand them an all-ones mask instead, so every
// object up to the consumer's count is drawn, and the CPU never reads GPU-written memory there. The
// buffer is one 64 KiB block from runtime_low_map (guest-visible, in the low range): 8192 qwords, 524288
// bits, more than the u16 object count the consumers read. The consumers only read the mask.
//
// Verify (SOTC_NATIVE=2): before the swap, the real masks are read through SotcGuest and the bits below
// the count that are 0 are counted (the objects the real path skips). Mode 2 keeps the old page tracking
// running underneath, so the backing of GPU-written pages holds what the old path read back.
//
// SOTC_NATIVE_MASKS=prev: the masks of C1, C2 and C3 (NATIVE_MASK_VIS, _VIS_IND, _DECAL) are served by
// SotcNativeMasks::Serve (native_masks.cpp). C4 (NATIVE_MASK_C4) is not served yet.
//
// Spin verify: the sites after each XPR label spin count the skips, and the skips where the label (its
// pointer is in rsi on the skip path) is not 1 yet. Only counted with SOTC_NATIVE_MASKS=all, because the
// spins are removed by byte patches only then, and rsi is not the label pointer on the wait path.
//
// Water verify (mode 2 only, the read is kept there): the store of the ring read counts the changes of
// +0x11c. No consumer reads +0x11c (gpuresults.md, Q1).
#include "native_results.h"
#include "../probe_xpr_groups.h" // sotc: SOTC_XPR_GROUPS (P0 measurement of the XPR draw loop)

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../guest_mem.h"
#include "../hooks.h"
#include "native_masks.h" // sotc: SOTC_NATIVE_MASKS=prev (the last completed masks)
#include "native.h" // sotc: SotcNative::Verify / VerifyGpuWait

// src/runtime_memory.c: a guest-visible block in the low range (the image and the stacks live there too).
// Weak, as bb_low_block in engine_cmd.cpp: the test binaries that link libbbgpu.a without the loader
// still link; without it the masks fallback stays off (the real masks are kept).
extern "C" void* runtime_low_map(size_t size, int prot) __attribute__((weak));

namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr int kProtReadWrite = 1 | 2; // PROT_READ | PROT_WRITE (src/compat_win.h)
constexpr uint64_t kOnesBytes = 64 * 1024;
constexpr uint64_t kOnesQwords = kOnesBytes / 8;
constexpr uint64_t kCountOffset = 0x64; // u16 object count at param_1 + 100 (C1, C2, C4)

// The mask consumers, in the order of their hooks (rdi = the object in all four).
enum Consumer : int { kVis = 0, kVisInd = 1, kDecal = 2, kC4 = 3, kConsumers = 4 };
constexpr const char* kConsumerHook[kConsumers] = {"NATIVE_MASK_VIS", "NATIVE_MASK_VIS_IND",
                                                   "NATIVE_MASK_DECAL", "NATIVE_MASK_C4"};
constexpr const char* kConsumerName[kConsumers] = {"vis", "vis_ind", "decal", "c4"};

// The spin continuation sites, in the order of their hooks.
enum Spin : int { kSpinDecal = 0, kSpinXpr = 1, kSpinCull = 2, kSpinCull2 = 3, kSpins = 4 };
constexpr const char* kSpinHook[kSpins] = {"NATIVE_SPIN_DECAL", "NATIVE_SPIN_XPR", "NATIVE_SPIN_CULL",
                                           "NATIVE_SPIN_CULL2"};
constexpr const char* kSpinName[kSpins] = {"decal", "xpr", "cull", "cull2"};
// Guest RIP of each continuation site (the expected offsets of the hooks in scripts/hooks.d/native_results.py).
constexpr uint64_t kSpinSite[kSpins] = {0x11e219, 0x11dc79, 0x11d839, 0x11db19};

struct ConsumerCounters {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> swapped{0};    // masks=all: calls that got the all-ones mask
    std::atomic<uint64_t> bad_count{0};  // count beyond the buffer (the real mask is kept)
    std::atomic<uint64_t> unreadable{0}; // verify: object or mask outside a mapped region
    std::atomic<uint64_t> bits{0};       // verify: bits below the count
    std::atomic<uint64_t> bits_zero{0};  // verify: of those, bits that were 0 in the real mask
    std::atomic<uint64_t> calls_zero{0}; // verify: calls with at least one zero bit
};
ConsumerCounters g_masks[kConsumers];

struct SpinCounters {
    std::atomic<uint64_t> skips{0};     // arrivals (only the skip path arrives when the spin is removed)
    std::atomic<uint64_t> not_ready{0}; // of those, the label was not 1 yet
    std::atomic<uint64_t> unreadable{0};
};
SpinCounters g_spins[kSpins];

std::atomic<uint64_t> g_water_stores{0};
std::atomic<uint64_t> g_water_changed{0};
std::atomic<uint64_t> g_water_unreadable{0};

// SOTC_NATIVE_MASKS: "all" is the all-visible fallback; anything else keeps the GPU masks.
bool MasksAll() {
    static const bool all = [] {
        const char* v = std::getenv("SOTC_NATIVE_MASKS");
        return v != nullptr && std::strcmp(v, "all") == 0;
    }();
    return all;
}

// SOTC_NATIVE=2: the verify counters run. The switch value of every native result hook is SOTC_NATIVE.
bool VerifyOn() {
    static const bool verify = SotcHooks::HookMode(kConsumerHook[kVis]) == 2;
    return verify;
}

// The all-ones mask, shared by every consumer and every call. Null when the block could not be mapped
// (the real masks are then kept).
const uint64_t* Ones() {
    static const uint64_t* const ones = []() -> const uint64_t* {
        void* p = runtime_low_map ? runtime_low_map(kOnesBytes, kProtReadWrite) : nullptr;
        if (p == nullptr) {
            return nullptr;
        }
        std::memset(p, 0xff, kOnesBytes);
        return static_cast<const uint64_t*>(p);
    }();
    return ones;
}

// The object count: u16 at param_1 + 0x64 for C1, C2 and C4; u32 at param_1 + 0 for the decal consumer.
bool ReadCount(int which, uint64_t object, uint64_t* count) {
    if (which == kDecal) {
        uint32_t n = 0;
        if (!SotcGuest::Read(object, &n, sizeof n)) {
            return false;
        }
        *count = n;
        return true;
    }
    uint16_t n = 0;
    if (!SotcGuest::Read(object + kCountOffset, &n, sizeof n)) {
        return false;
    }
    *count = n;
    return true;
}

// Verify: the bits below the count that the real mask(s) hold as 0. C2 (two masks) draws an object only
// when both masks hold its bit, so a bit is 0 when either mask has it 0.
void CountRealMasks(uint64_t count, uint64_t mask_a, uint64_t mask_b, ConsumerCounters& c) {
    const uint64_t qwords = (count + 63) / 64;
    const uint64_t bytes = qwords * 8;
    const unsigned char* a = SotcGuest::Backing(mask_a, bytes);
    const unsigned char* b = mask_b != 0 ? SotcGuest::Backing(mask_b, bytes) : nullptr;
    if (a == nullptr || (mask_b != 0 && b == nullptr)) {
        c.unreadable++;
        return;
    }
    uint64_t zero = 0;
    for (uint64_t w = 0; w < qwords; ++w) {
        uint64_t valid = ~uint64_t{0};
        if (w == qwords - 1 && (count % 64) != 0) {
            valid = (uint64_t{1} << (count % 64)) - 1;
        }
        uint64_t real = 0;
        std::memcpy(&real, a + w * 8, 8);
        if (b != nullptr) {
            uint64_t second = 0;
            std::memcpy(&second, b + w * 8, 8);
            real &= second;
        }
        zero += static_cast<uint64_t>(std::popcount(~real & valid));
    }
    c.bits += count;
    c.bits_zero += zero;
    if (zero != 0) {
        c.calls_zero++;
    }
}

// Entry of the four mask consumers. rdi = the object; the mask is rsi (C1, C3, C4) or rsi and rdx (C2).
HookAction OnMaskConsumeImpl(GuestRegs& regs, void* user) {
    const int which = static_cast<int>(reinterpret_cast<uintptr_t>(user));
    ConsumerCounters& c = g_masks[which];
    c.calls++;
    // sotc: SOTC_NATIVE_MASKS=prev: the main-scene and decal consumers read the last completed copy (native_masks.h).
    // The decal consumer (C3) has one mask, in rsi; its count is the u32 at the decal object (ReadCount).
    if (SotcNativeMasks::Prev() && (which == kVis || which == kVisInd || which == kDecal)) {
        uint64_t count = 0;
        if (ReadCount(which, regs.rdi, &count) && count != 0) {
            regs.rsi = SotcNativeMasks::Serve(regs.rsi, count);
            if (which == kVisInd) {
                regs.rdx = SotcNativeMasks::Serve(regs.rdx, count);
            }
            c.swapped++;
        }
        return HookAction::Continue;
    }
    const bool verify = VerifyOn();
    if (!MasksAll() && !verify) {
        return HookAction::Continue;
    }
    uint64_t count = 0;
    if (!ReadCount(which, regs.rdi, &count)) {
        c.unreadable++;
        return HookAction::Continue;
    }
    if (count == 0) {
        return HookAction::Continue; // the consumer returns before it reads a mask
    }
    const uint64_t qwords = (count + 63) / 64;
    if (qwords > kOnesQwords) {
        c.bad_count++;
        return HookAction::Continue;
    }
    const bool two_masks = which == kVisInd;
    if (verify) {
        CountRealMasks(count, regs.rsi, two_masks ? regs.rdx : 0, c);
    }
    if (!MasksAll()) {
        return HookAction::Continue;
    }
    const uint64_t* ones = Ones();
    if (ones == nullptr) {
        return HookAction::Continue;
    }
    regs.rsi = reinterpret_cast<uint64_t>(ones);
    if (two_masks) {
        regs.rdx = reinterpret_cast<uint64_t>(ones);
    }
    c.swapped++;
    return HookAction::Continue;
}

// sotc: SOTC_XPR_GROUPS (probe_xpr_groups.h): the P0 measurement of the XPR draw loop reads the mask the loop will
// read, so it runs after the mask swap above. Observe only.
HookAction OnMaskConsume(GuestRegs& regs, void* user) {
    const HookAction action = OnMaskConsumeImpl(regs, user);
    const int which = static_cast<int>(reinterpret_cast<uintptr_t>(user));
    if (which == kVis) {
        SotcXprGroups::OnConsume(regs);
    } else if (which == kVisInd) {
        SotcXprGroups::OnConsumeIndirect();
    }
    return action;
}

// Site after an XPR label spin (the skip target). rsi holds the label pointer on the skip path.
HookAction OnSpinSite(GuestRegs& regs, void* user) {
    const int which = static_cast<int>(reinterpret_cast<uintptr_t>(user));
    // sotc: verify of the guest label wait. For the XPR spin the site (0x11dc79) is reached after the label
    // loop exits and after the job-wait path (checked in the disassembly; the other three sites are assumed the
    // same shape). With the spin removed (masks=all) no wait happens, so nothing is counted. rsi is the label
    // pointer on the loop path; the job-wait path may leave another value in it.
    if (SotcNative::Verify() && !MasksAll()) {
        SotcNative::VerifyGpuWait(regs.rsi, kSpinSite[which]);
    }
    if (!MasksAll()) {
        return HookAction::Continue; // the spin is only removed with SOTC_NATIVE_MASKS=all
    }
    SpinCounters& s = g_spins[which];
    s.skips++;
    uint64_t label = 0;
    if (!SotcGuest::Read(regs.rsi, &label, sizeof label)) {
        s.unreadable++;
    } else if (label != 1) {
        s.not_ready++;
    }
    return HookAction::Continue;
}

// Site of the store of the water ring read: mov dword [r14+0x11c], eax (eax = rax, the new value).
HookAction OnWaterStore(GuestRegs& regs, void*) {
    if (!VerifyOn()) {
        return HookAction::Continue; // the read is skipped with SOTC_NATIVE=1, so this is not reached
    }
    g_water_stores++;
    uint32_t old_value = 0;
    if (!SotcGuest::Read(regs.r14 + 0x11c, &old_value, sizeof old_value)) {
        g_water_unreadable++;
        return HookAction::Continue;
    }
    const uint32_t new_value = static_cast<uint32_t>(regs.rax);
    if (old_value != new_value) {
        g_water_changed++;
    }
    return HookAction::Continue;
}

uint64_t Take(std::atomic<uint64_t>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
}

} // namespace

namespace SotcNativeResults {

void Register() {
    for (int i = 0; i < kConsumers; ++i) {
        SotcHooks::Handler handler;
        handler.pre = &OnMaskConsume;
        handler.post = nullptr;
        handler.user = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        SotcHooks::RegisterHandler(kConsumerHook[i], handler);
    }
    for (int i = 0; i < kSpins; ++i) {
        SotcHooks::Handler handler;
        handler.pre = &OnSpinSite;
        handler.post = nullptr;
        handler.user = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        SotcHooks::RegisterHandler(kSpinHook[i], handler);
    }
    SotcHooks::Handler water;
    water.pre = &OnWaterStore;
    water.post = nullptr;
    water.user = nullptr;
    SotcHooks::RegisterHandler("NATIVE_WATER_STORE", water);
    SotcNativeMasks::Register(); // sotc: NATIVE_MASK_PRODUCE (SOTC_NATIVE_MASKS=prev)
}

void Report(std::string& out, double seconds) {
    bool any = false;
    for (const char* name : kConsumerHook) {
        any = any || SotcHooks::IsInstalled(name);
    }
    if (!any) {
        return;
    }
    char line[512];
    std::snprintf(line, sizeof line, "SOTC NATIVE_RESULTS mode=%d masks=%s window=%.1fs", SotcHooks::HookMode(kConsumerHook[kVis]),
                  MasksAll() ? "all" : SotcNativeMasks::Prev() ? "prev" : "gpu", seconds);
    out += line;
    out += '\n';
    SotcNativeMasks::Report(out, seconds);
    for (int i = 0; i < kConsumers; ++i) {
        ConsumerCounters& c = g_masks[i];
        if (!SotcHooks::IsInstalled(kConsumerHook[i])) {
            continue;
        }
        std::snprintf(line, sizeof line,
                      "  masks %s: calls %llu swapped %llu, verify bits %llu zero %llu calls with zero %llu, "
                      "unreadable %llu bad count %llu",
                      kConsumerName[i], static_cast<unsigned long long>(Take(c.calls)),
                      static_cast<unsigned long long>(Take(c.swapped)),
                      static_cast<unsigned long long>(Take(c.bits)),
                      static_cast<unsigned long long>(Take(c.bits_zero)),
                      static_cast<unsigned long long>(Take(c.calls_zero)),
                      static_cast<unsigned long long>(Take(c.unreadable)),
                      static_cast<unsigned long long>(Take(c.bad_count)));
        out += line;
        out += '\n';
    }
    for (int i = 0; i < kSpins; ++i) {
        SpinCounters& s = g_spins[i];
        if (!SotcHooks::IsInstalled(kSpinHook[i])) {
            continue;
        }
        std::snprintf(line, sizeof line, "  spin %s: skips %llu label not ready %llu unreadable %llu",
                      kSpinName[i], static_cast<unsigned long long>(Take(s.skips)),
                      static_cast<unsigned long long>(Take(s.not_ready)),
                      static_cast<unsigned long long>(Take(s.unreadable)));
        out += line;
        out += '\n';
    }
    if (SotcHooks::IsInstalled("NATIVE_WATER_STORE")) {
        std::snprintf(line, sizeof line, "  water ring read: stores %llu value changed %llu unreadable %llu",
                      static_cast<unsigned long long>(Take(g_water_stores)),
                      static_cast<unsigned long long>(Take(g_water_changed)),
                      static_cast<unsigned long long>(Take(g_water_unreadable)));
        out += line;
        out += '\n';
    }
}

} // namespace SotcNativeResults
