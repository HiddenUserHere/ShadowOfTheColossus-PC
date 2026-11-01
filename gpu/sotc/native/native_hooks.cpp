// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native write announces (docs/re-stutters/native/PLAN.md N2, declarations in
// scripts/hooks.d/native_writes.py, switch SOTC_NATIVE). Every hook is a Site hook placed after the
// engine finished a write (or before a release), so the announce always follows the copy. Handlers only
// read registers and the guest words they name, then call SotcNative::Announce / Release (native.h).
// Every handler returns Continue: the displaced instruction runs next, unchanged.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

// Reads a guest u32; 0 when the address is unmapped (the announce is then skipped).
uint32_t GuestU32(uint64_t address) {
    const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(address));
    if (!p) {
        return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// Per-window coverage counters (verify report: bytes announced per class per window).
struct Window {
    std::atomic<uint64_t> res_announces{0}, res_bytes{0};
    std::atomic<uint64_t> res_gpu_bytes{0}, res_cpu_bytes{0};
    std::atomic<uint64_t> tex_announces{0}, tex_bytes{0};
    std::atomic<uint64_t> mip_announces{0}, mip_bytes{0};
    std::atomic<uint64_t> mip_releases{0}, mip_release_bytes{0};
    std::atomic<uint64_t> skipped{0};
};
Window g_window;

void Count(std::atomic<uint64_t>& announces, std::atomic<uint64_t>& bytes, uint64_t size) {
    announces.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
}

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

// Report line for the 5 s window (SotcNative::Report prints the rest). Resets the counters.
void Report(std::string& out, double seconds) {
    const auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const uint64_t ra = take(g_window.res_announces), rb = take(g_window.res_bytes);
    const uint64_t rg = take(g_window.res_gpu_bytes), rc = take(g_window.res_cpu_bytes);
    const uint64_t ta = take(g_window.tex_announces), tb = take(g_window.tex_bytes);
    const uint64_t ma = take(g_window.mip_announces), mb = take(g_window.mip_bytes);
    const uint64_t mr = take(g_window.mip_releases), mrb = take(g_window.mip_release_bytes);
    const uint64_t sk = take(g_window.skipped);
    const double mib = 1024.0 * 1024.0;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "SOTC NATIVE WRITES res %llu announces %.2f MiB (gpu %.2f cpu %.2f) tex %llu %.2f MiB "
                  "mip %llu %.2f MiB rel %llu %.2f MiB skipped %llu per %.1f s\n",
                  static_cast<unsigned long long>(ra), rb / mib, rg / mib, rc / mib,
                  static_cast<unsigned long long>(ta), tb / mib, static_cast<unsigned long long>(ma),
                  mb / mib, static_cast<unsigned long long>(mr), mrb / mib,
                  static_cast<unsigned long long>(sk), seconds);
    out += line;
}

void Register() {
    SotcHooks::RegisterHandler("NATIVE_RES_COPY", {ResCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_TEX_COPY", {TexCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_TEX_TAIL_COPY", {TexTailCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MIP_COPY", {MipCopy, nullptr, nullptr});
    SotcHooks::RegisterHandler("NATIVE_MIP_RELEASE", {MipRelease, nullptr, nullptr});
}

} // namespace SotcNativeHooks
