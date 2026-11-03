// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: frame ring probe (SOTC_FRAME_RINGS, docs/re-backend/10-ring-reuse.md).
// One Entry hook, RING_INIT (scripts/hooks.d/frame_rings.py), on the ring allocator init 0x481f0.
// Arguments (SysV): rdi = ring object, esi = slot count, rdx = chunk marker array, rcx = block base,
// r8 = block size. The ring object's name is a const char* at +0x10 (stored by the ctor 0x475c0
// before the init call). Only "RenderContextFrameCPU" and "RenderContextFrameGPU" are kept.
//
// Counters per 5 s window: fast ops F = RecordCurrentWrite calls whose range lies fully inside the
// CPU ring; verify X = F minus the ones that came from the CPU copy path (NativeExactUpload).
// Nothing is skipped in any mode: the counts say how many GPU-tracked writes the CPU ring would have.

#include "probe_frame_rings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "guest_mem.h"
#include "hooks.h"

namespace SotcFrameRings {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kNameField = 0x10;
constexpr const char* kCpuName = "RenderContextFrameCPU";
constexpr const char* kGpuName = "RenderContextFrameGPU";

std::mutex g_init_mutex; // ring registration only (once per render context init)
std::atomic<uint64_t> g_cpu_begin{0}, g_cpu_end{0}; // [begin, end) of the CPU ring; end 0 = none
std::atomic<uint64_t> g_gpu_begin{0}, g_gpu_end{0};
std::atomic<int> g_rings_found{0};
std::atomic<uint64_t> g_fast_ops{0}, g_cpu_path_ops{0};

int ModeOnce() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_FRAME_RINGS");
        const int n = (v != nullptr) ? std::atoi(v) : 0;
        return n >= 2 ? 2 : (n >= 1 ? 1 : 0);
    }();
    return mode;
}

HookAction RingInit(GuestRegs& regs, void*) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 24) { // sotc: diagnostic, the first ring inits
        uint64_t q[4] = {};
        SotcGuest::Read(regs.rdi, q, sizeof(q));
        std::fprintf(stderr, "SOTC FRAME RINGS init: rdi %#llx rsi %#llx rdx %#llx rcx %#llx r8 %#llx obj %#llx %#llx %#llx %#llx\n",
                     (unsigned long long)regs.rdi, (unsigned long long)regs.rsi, (unsigned long long)regs.rdx,
                     (unsigned long long)regs.rcx, (unsigned long long)regs.r8, (unsigned long long)q[0],
                     (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3]);
    }
    uint64_t name_ptr = 0;
    if (!SotcGuest::Read(regs.rdi + kNameField, &name_ptr, sizeof(name_ptr)) || name_ptr == 0) {
        return HookAction::Continue;
    }
    char name[32] = {};
    if (!SotcGuest::Read(name_ptr, name, sizeof(name) - 1)) {
        // The name is a string literal in the eboot's read-only data, which SotcGuest does not cover.
        const uint64_t image = static_cast<uint64_t>(SotcHooks::ImageBase());
        if (image == 0 || name_ptr < image || name_ptr - image >= 0x10000000) {
            return HookAction::Continue;
        }
        std::memcpy(name, reinterpret_cast<const void*>(name_ptr), sizeof(name) - 1);
    }
    const uint64_t base = regs.rcx;
    const uint64_t size = regs.r8;
    if (size == 0) {
        return HookAction::Continue;
    }
    std::lock_guard<std::mutex> lock(g_init_mutex);
    std::fprintf(stderr, "SOTC FRAME RINGS ring %s base %#llx size %#llx\n", name, (unsigned long long)base,
                 (unsigned long long)size);
    if (std::strcmp(name, kCpuName) == 0) {
        g_cpu_begin.store(base, std::memory_order_relaxed);
        g_cpu_end.store(base + size, std::memory_order_release);
        g_rings_found.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(name, kGpuName) == 0) {
        g_gpu_begin.store(base, std::memory_order_relaxed);
        g_gpu_end.store(base + size, std::memory_order_release);
        g_rings_found.fetch_add(1, std::memory_order_relaxed);
    }
    return HookAction::Continue;
}

bool InCpuRing(uint64_t addr, uint64_t size) {
    const uint64_t end = g_cpu_end.load(std::memory_order_acquire);
    if (end == 0 || size == 0) {
        return false;
    }
    const uint64_t begin = g_cpu_begin.load(std::memory_order_relaxed);
    return addr >= begin && addr + size <= end;
}

} // namespace

void Register() {
    SotcHooks::Handler ring;
    ring.pre = RingInit;
    SotcHooks::RegisterHandler("RING_INIT", ring);
}

int Mode() {
    return ModeOnce();
}

// sotc: SOTC_VSIZE_PROBE. 1 inside the CPU frame ring, 2 inside the GPU one, 0 outside both, -1 when no ring
// was reported (RING_INIT needs SOTC_FRAME_RINGS=1 to dispatch).
int RingOf(uint64_t addr, uint64_t size) {
    if (g_rings_found.load(std::memory_order_relaxed) == 0) {
        return -1;
    }
    if (InCpuRing(addr, size)) {
        return 1;
    }
    const uint64_t end = g_gpu_end.load(std::memory_order_acquire);
    const uint64_t begin = g_gpu_begin.load(std::memory_order_relaxed);
    if (end != 0 && size != 0 && addr >= begin && addr + size <= end) {
        return 2;
    }
    return 0;
}

void NoteWrite(uint64_t addr, uint64_t size) {
    if (ModeOnce() == 0 || !InCpuRing(addr, size)) {
        return;
    }
    g_fast_ops.fetch_add(1, std::memory_order_relaxed);
}

void NoteCpuPath(uint64_t addr, uint64_t size) {
    if (ModeOnce() == 0 || !InCpuRing(addr, size)) {
        return;
    }
    g_cpu_path_ops.fetch_add(1, std::memory_order_relaxed);
}

void Report(std::string& out, double seconds) {
    const int mode = ModeOnce();
    if (mode == 0 || seconds <= 0.0) {
        return;
    }
    const uint64_t fast = g_fast_ops.exchange(0, std::memory_order_relaxed);
    const uint64_t cpu_path = g_cpu_path_ops.exchange(0, std::memory_order_relaxed);
    const uint64_t verify = fast >= cpu_path ? fast - cpu_path : 0;
    const uint64_t cpu_b = g_cpu_begin.load(std::memory_order_relaxed);
    const uint64_t cpu_e = g_cpu_end.load(std::memory_order_relaxed);
    const uint64_t gpu_b = g_gpu_begin.load(std::memory_order_relaxed);
    const uint64_t gpu_e = g_gpu_end.load(std::memory_order_relaxed);
    char line[512];
    std::snprintf(line, sizeof(line),
                  "SOTC FRAME RINGS: rings found %d (cpu base 0x%llx size 0x%llx, gpu base 0x%llx size 0x%llx), "
                  "fast ops %llu, verify gpu-writes-in-cpu-ring %llu (mode %d%s)\n",
                  g_rings_found.load(std::memory_order_relaxed), static_cast<unsigned long long>(cpu_b),
                  static_cast<unsigned long long>(cpu_e - cpu_b), static_cast<unsigned long long>(gpu_b),
                  static_cast<unsigned long long>(gpu_e - gpu_b), static_cast<unsigned long long>(fast),
                  static_cast<unsigned long long>(verify), mode,
                  mode == 1 ? ", measure only: fast path not implemented" : "");
    out += line;
}

} // namespace SotcFrameRings
