// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU-GPU synchronization of the PC-native engine (docs/re-stutters/native/sync.md section 6).
//
// Frame numbering: g_closed is the ticket of the last frame the guest closed with sceGnmSubmitDone
// (FrameSubmitted). While the guest builds frame closed + 1, it writes the DCB / frame-ring slot that
// frame closed - 1 used (the engine's ring has 2 slots, framedata.md: renderer+0x7b0 toggles per frame).
// So the only wait the native path needs before building is FrameWait(closed - 1). The engine's
// frame-slot spin (RenderBackend_Frame, 0x90ea0, the label compare at 0x90f47) waits for the same frame:
// its label is the EOP of the frame the slot held two frames back, so the hook calls the same ticket
// and forces the compare to equal, which skips the spin loop.
#include "native_sync.h"
#include "verify.h"

#include <atomic>
#include <cstdio>

#include "../hooks.h"
#include "native.h"

namespace SotcNativeSync {
namespace {

std::atomic<uint64_t> g_closed{0};       // ticket of the last frame closed by sceGnmSubmitDone
std::atomic<uint64_t> c_reuse_calls{0};  // WaitReuse calls
std::atomic<uint64_t> c_reuse_waits{0};  // WaitReuse calls that asked FrameWait (target >= 1)
std::atomic<uint64_t> c_slot_spin{0};    // 0x90f47 hook hits (frame-slot spin of 0x90ea0)
std::atomic<uint64_t> c_slot_spin_wait{0};
std::atomic<uint64_t> c_subs_calls{0};   // sceGnmAreSubmitsAllowed calls
std::atomic<uint64_t> c_subs_denied{0};  // ... that answered 0 (frame slot still busy)

// Frame two back is the one whose slot the next frame reuses; frame 0 means nothing was closed.
uint64_t ReuseTarget() {
    const uint64_t closed = g_closed.load(std::memory_order_acquire);
    return closed >= 2 ? closed - 1 : 0;
}

// Pre handler of NATIVE_FRAME_SLOT_SPIN (site at 0x90f47: `cmp rdx, [r13 + rcx*8 + 0x780]`, 8 bytes,
// the first compare of RenderBackend_Frame's slot spin). rcx is the slot index, r13 the renderer.
// The FrameWait covers what the spin waited for; the displaced cmp then compares rdx with the
// expected value itself, so the spin loop (0x90f60..0x90f74) is never entered. Continue runs the cmp.
SotcHooks::HookAction SlotSpinPre(SotcHooks::GuestRegs& regs, void*) {
    c_slot_spin.fetch_add(1, std::memory_order_relaxed);
    const uint64_t target = ReuseTarget();
    if (target != 0) {
        c_slot_spin_wait.fetch_add(1, std::memory_order_relaxed);
        SotcNative::FrameWait(target);
    }
    const auto* expected =
        reinterpret_cast<const uint64_t*>(regs.r13 + regs.rcx * 8 + 0x780); // engine's CPU copy
    regs.rdx = *expected;
    return SotcHooks::HookAction::Continue;
}

} // namespace

void OnSubmitDone() {
    g_closed.store(SotcNative::FrameSubmitted(), std::memory_order_release);
    SotcVerify::OnSubmit(); // sotc: resolve the coverage oracle's pending faults once per frame
}

void WaitReuse() {
    c_reuse_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t target = ReuseTarget();
    if (target != 0) {
        c_reuse_waits.fetch_add(1, std::memory_order_relaxed);
        SotcNative::FrameWait(target);
    }
}

bool SubmitsAllowed() {
    c_subs_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t target = ReuseTarget();
    if (target == 0 || SotcNative::FrameCompleted() >= target) {
        return true;
    }
    c_subs_denied.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Report(std::string& out, double seconds) {
    char line[320];
    const uint64_t reuse_calls = c_reuse_calls.exchange(0);
    const uint64_t reuse_waits = c_reuse_waits.exchange(0);
    const uint64_t spin = c_slot_spin.exchange(0);
    const uint64_t spin_waits = c_slot_spin_wait.exchange(0);
    const uint64_t subs = c_subs_calls.exchange(0);
    const uint64_t denied = c_subs_denied.exchange(0);
    std::snprintf(line, sizeof line,
                  "SOTC NATIVE SYNC %.1fs closed %llu reuse %llu/%llu slot_spin %llu/%llu allowed %llu denied %llu\n",
                  seconds, static_cast<unsigned long long>(g_closed.load()),
                  static_cast<unsigned long long>(reuse_waits), static_cast<unsigned long long>(reuse_calls),
                  static_cast<unsigned long long>(spin_waits), static_cast<unsigned long long>(spin),
                  static_cast<unsigned long long>(subs - denied), static_cast<unsigned long long>(denied));
    out += line;
}

void Register() {
    SotcHooks::RegisterHandler("NATIVE_FRAME_SLOT_SPIN", SotcHooks::Handler{SlotSpinPre, nullptr, nullptr});
}

} // namespace SotcNativeSync
