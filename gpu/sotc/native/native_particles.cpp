// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native particle stats, option 3 of docs/re-stutters/native/impl-particles.md. See
// native_particles.h for the contract.
//
// Timeline (particle_prev.cpp, checked against the disassembly). The call with pool counter c (after its
// increment) dispatches the count pass for call id c and reads the array dispatched by call id c - 2.
// Each array index is a system index of the pool, so a copy of any earlier dispatch answers the same
// question with older data.
//
// Copies: native_copies.h, key = the pool, seq = the call id. Serving: the copy of id c - 2 when complete (the
// data the exact read sees); else the newest complete one of ids c - 3 .. c - 4 (one or two calls older, inside
// the 1 s retirement tolerance); else the exact read. A copy is served only when it holds every system the loop
// reads (extent): a value the copy does not hold is never served, so no unknown count becomes a zero that would
// retire a live AutoDestroy system. The counts go into one of 16 guest blocks (low range) and 0x8c1b0 returns
// that block; ProcessStats only reads the array (decomp 0x1018c0), so nothing the game writes is lost.
//
// Verify (SotcNative::Verify): nothing is served. The candidate's counts are kept for the call, and the
// site after the load at 0x101a98 compares each exact count (r15d) with them.
//
// What the count feeds on the CPU (decomp, checked; the served value never reaches the GPU):
// - ProcessStats retirement: count < 1 and the AutoDestroy grace (1 s since creation, or the forced flag)
//   releases the system; the owner then sees an invalid handle.
// - The system record +0xb4 (FUN_000fc2d0). Its only reader is FUN_000fc0a0 (scan of out/re/decomp), called by
//   FUN_003096d0 (OnParticleCountBegin 0xb41adbdb / OnParticleCountEnd 0x7eb63229 events, kill event 0x3dbe79b1
//   when the handle is gone), FUN_003bca70 (SetParticleCountVar: a float script variable) and FUN_003b0990
//   (start/stop flag, ParticleCount).
// - Draw submission (ParticleRenderer RenderTranslucent / RenderModels / PrepForRendering / UpdateSimulation) does
//   not read the record: the particle draws use GPU-side data only.
// So a served count that is one or two calls old moves those events, script variables and retirements by one or
// two frames; it cannot change what the particle renderer draws. Counters: "older" and "fallback" in the report.
#include "native_particles.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "native.h"
#include "native_copies.h"

// src/runtime_memory.c: a guest-visible block in the low range. Weak, as in native_results.cpp: the test
// binaries that link libbbgpu.a without the loader still link; without it nothing is served.
extern "C" void* runtime_low_map(size_t size, int prot) __attribute__((weak));

namespace SotcNativeParticles {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kVerifyHook = "NATIVE_PARTICLE_VERIFY";
constexpr int kProtReadWrite = 1 | 2; // PROT_READ | PROT_WRITE (src/compat_win.h)
constexpr uint64_t kMaxEntries = 4096; // u32 counts per copy (16 KiB); larger arrays keep the exact read
constexpr uint64_t kPadEntries = 256;  // systems created after the dispatch, before its read
constexpr uint64_t kServeBlocks = 16;
constexpr uint64_t kServeBlockBytes = kMaxEntries * 4;
constexpr uint64_t kOldestAge = 4; // serve ids c - 2 .. c - 4

std::atomic<uint64_t> g_next_block{0};

struct Counters {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> registered{0};
    std::atomic<uint64_t> too_large{0};    // arrays past kMaxEntries (exact read)
    std::atomic<uint64_t> served_exact{0}; // id c - 2: the same data as the exact read
    std::atomic<uint64_t> served_older{0}; // id c - 3 or c - 4
    std::atomic<uint64_t> fallback{0};     // no complete copy: the exact read
    std::atomic<uint64_t> not_copied{0};   // ... of which the c - 2 copy had no GPU write at all
    std::atomic<uint64_t> empty{0};        // calls that read no system
    std::atomic<uint64_t> no_native{0};    // armed without SOTC_NATIVE
    std::atomic<uint64_t> v_counts{0};     // verify: counts compared
    std::atomic<uint64_t> v_mismatch{0};   // verify: counts that differ
    std::atomic<uint64_t> v_mismatch_exact{0}; // ... of which from the c - 2 copy (must be 0)
    std::atomic<uint64_t> v_zero{0};       // verify: copy 0 where the exact count is > 0
};
Counters g_c;

// Verify: the candidate of the current call (the site handler runs on the same thread, later).
struct VerifyCall {
    bool active = false;
    bool exact = false;
    uint64_t pool = 0;
    std::vector<uint32_t> counts;
};
thread_local VerifyCall t_verify;

uint64_t AlignUp(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

uint8_t* Blocks() {
    static uint8_t* const blocks = []() -> uint8_t* {
        void* p = runtime_low_map ? runtime_low_map(kServeBlocks * kServeBlockBytes, kProtReadWrite) : nullptr;
        return static_cast<uint8_t*>(p);
    }();
    return blocks;
}

HookAction OnVerifySite(GuestRegs& regs, void*) {
    VerifyCall& v = t_verify;
    if (!v.active || regs.r13 != v.pool || regs.rbx >= v.counts.size()) {
        return HookAction::Continue;
    }
    const uint32_t exact = static_cast<uint32_t>(regs.r15);
    const uint32_t copy = v.counts[regs.rbx];
    g_c.v_counts.fetch_add(1, std::memory_order_relaxed);
    if (copy != exact) {
        g_c.v_mismatch.fetch_add(1, std::memory_order_relaxed);
        if (v.exact) {
            g_c.v_mismatch_exact.fetch_add(1, std::memory_order_relaxed);
        }
        if (copy == 0 && static_cast<int32_t>(exact) > 0) {
            g_c.v_zero.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return HookAction::Continue;
}

uint64_t Take(std::atomic<uint64_t>& c) {
    return c.exchange(0, std::memory_order_relaxed);
}

} // namespace

bool Armed() {
    static const bool armed = [] {
        const char* v = std::getenv("SOTC_NATIVE_PARTICLES");
        return v != nullptr && std::strcmp(v, "prev") == 0;
    }();
    return armed;
}

HookAction OnStatsRead(GuestRegs& regs, uint64_t pool, uint64_t counter, uint64_t dispatch_base,
                       uint64_t dispatched, uint64_t extent) {
    t_verify.active = false;
    if (!Armed()) {
        return HookAction::Continue;
    }
    g_c.calls.fetch_add(1, std::memory_order_relaxed);
    if (!SotcNative::On()) {
        g_c.no_native.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    const uint64_t key = SotcNativeCopies::kKeyParticles | pool;
    // The array this call's pass writes, read two calls later. Only what the loop can read is copied.
    if (dispatch_base != 0 && dispatched != 0) {
        uint64_t entries = AlignUp(extent + kPadEntries, 64);
        entries = entries < dispatched ? entries : dispatched;
        if (entries > kMaxEntries) {
            g_c.too_large.fetch_add(1, std::memory_order_relaxed);
        } else if (SotcNativeCopies::Register(key, counter, dispatch_base, entries * 4)) {
            g_c.registered.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (counter < 3) {
        return HookAction::Continue; // ProcessStats does not call 0x8c1b0 then; kept for safety
    }
    if (extent == 0) {
        g_c.empty.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue; // the loop reads nothing
    }
    uint8_t* blocks = Blocks();
    if (!blocks || extent > kMaxEntries) {
        g_c.fallback.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    const bool verify = SotcNative::Verify();
    uint8_t* block = blocks + (g_next_block.fetch_add(1, std::memory_order_relaxed) % kServeBlocks) * kServeBlockBytes;
    std::vector<uint32_t>& counts = t_verify.counts;
    if (verify) {
        counts.resize(extent);
    }
    void* dst = verify ? static_cast<void*>(counts.data()) : static_cast<void*>(block);
    bool uncopied = false;
    const uint64_t seq =
        SotcNativeCopies::CopyOut(key, counter > kOldestAge ? counter - kOldestAge : 0, counter - 2, extent * 4, dst, &uncopied);
    if (seq == 0) {
        g_c.fallback.fetch_add(1, std::memory_order_relaxed);
        if (uncopied) {
            g_c.not_copied.fetch_add(1, std::memory_order_relaxed);
        }
        return HookAction::Continue;
    }
    const bool exact = seq + 2 == counter;
    if (verify) {
        t_verify.active = true;
        t_verify.exact = exact;
        t_verify.pool = pool;
        return HookAction::Continue;
    }
    (exact ? g_c.served_exact : g_c.served_older).fetch_add(1, std::memory_order_relaxed);
    regs.rax = reinterpret_cast<uint64_t>(block);
    return HookAction::Return;
}

void Register() {
    SotcHooks::Handler handler;
    handler.pre = &OnVerifySite;
    handler.post = nullptr;
    handler.user = nullptr;
    SotcHooks::RegisterHandler(kVerifyHook, handler);
}

void Report(std::string& out, double seconds) {
    if (!Armed()) {
        return;
    }
    const double secs = seconds > 0.0 ? seconds : 1.0;
    const uint64_t calls = Take(g_c.calls);
    char line[700];
    std::snprintf(line, sizeof line,
                  "SOTC NATIVE_PARTICLES %s: calls %.1f/s, served same %llu older %llu, exact reads %llu (no copy "
                  "%llu), empty %llu, registered %llu (too large %llu), no SOTC_NATIVE %llu; verify counts %llu "
                  "mismatched %llu (same-call %llu, must be 0; copy 0 for a live count %llu)\n",
                  SotcNative::Verify() ? "verify" : "prev", static_cast<double>(calls) / secs,
                  static_cast<unsigned long long>(Take(g_c.served_exact)),
                  static_cast<unsigned long long>(Take(g_c.served_older)),
                  static_cast<unsigned long long>(Take(g_c.fallback)),
                  static_cast<unsigned long long>(Take(g_c.not_copied)),
                  static_cast<unsigned long long>(Take(g_c.empty)),
                  static_cast<unsigned long long>(Take(g_c.registered)),
                  static_cast<unsigned long long>(Take(g_c.too_large)),
                  static_cast<unsigned long long>(Take(g_c.no_native)),
                  static_cast<unsigned long long>(Take(g_c.v_counts)),
                  static_cast<unsigned long long>(Take(g_c.v_mismatch)),
                  static_cast<unsigned long long>(Take(g_c.v_mismatch_exact)),
                  static_cast<unsigned long long>(Take(g_c.v_zero)));
    out += line;
}

} // namespace SotcNativeParticles
