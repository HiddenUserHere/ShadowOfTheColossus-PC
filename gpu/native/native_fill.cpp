// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_fill implementation (gap N2-10, see native_fill.h).
#include "native/native_fill.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Native::Bind::Fill {

namespace {

/// The pending ring. A draw's derivation runs some time before (or after) the command processor
/// builds the pipeline for the same shader set, and a scene can hold hundreds of distinct
/// materials, so the window is bounded: the newest 64 distinct (signature, key) pairs survive.
constexpr size_t kRingCapacity = 64;

struct Entry {
    StageSig sig{};
    Key128 key{};
};

std::mutex g_mutex;
std::array<Entry, kRingCapacity> g_ring{};
size_t g_head{0};  ///< index of the oldest entry
size_t g_count{0}; ///< live entries

std::atomic<u64> g_registered{0};
std::atomic<u64> g_evicted{0};
std::atomic<u64> g_attempts{0};
std::atomic<u64> g_matched{0};
std::atomic<u64> g_ambiguous{0};
std::atomic<u64> g_unmatched{0};

/// Trace gate of its own: native_fill must not depend on native_hooks.cpp (the command processor
/// has no reason to link the hook callbacks in), so the flag and the budget are read here.
bool Verbose() {
    static const bool v = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    return v;
}

std::atomic<u64> g_trace_budget{64};

bool TraceAllowed() {
    return g_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

Entry& At(size_t i) {
    return g_ring[(g_head + i) % kRingCapacity];
}

} // namespace

StageSig SigFromKey(const MaterialKey& key) {
    StageSig sig{};
    for (u32 s = 1; s < kNumStages; ++s) {
        sig.va[s] = key.stage[s].present != 0 ? key.stage[s].code_va : 0u;
    }
    // The CS slot (0) stays empty: the fill side resolves GRAPHICS pipelines only, and the
    // command processor cannot name the compute program (the guest's compute bind writes the CS
    // program into the queue's compute state, never into liverpool->regs). The guest's CS record
    // is staged by the compute bind (0x8616f0) and would otherwise poison every graphics
    // signature with a value the command processor can never match.
    return sig;
}

void RegisterPending(const StageSig& sig, const Key128& key) {
    g_registered.fetch_add(1, std::memory_order_relaxed);
    bool evicted = false;
    bool added = false;
    {
        std::scoped_lock lock{g_mutex};
        for (size_t i = 0; i < g_count; ++i) {
            const Entry& e = At(i);
            if (e.sig == sig && e.key == key) {
                return; // the same draw derived again: already pending
            }
        }
        if (g_count == kRingCapacity) {
            g_head = (g_head + 1) % kRingCapacity;
            --g_count;
            evicted = true;
        }
        At(g_count) = Entry{sig, key};
        ++g_count;
        added = true;
    }
    if (evicted) {
        g_evicted.fetch_add(1, std::memory_order_relaxed);
    }
    if (added && Verbose() && TraceAllowed()) {
        std::fprintf(stderr,
                     "[native-fill] pending ps=%08x vs=%08x gs=%08x es=%08x hs=%08x ls=%08x "
                     "key=%016llx%016llx\n",
                     sig.va[1], sig.va[2], sig.va[3], sig.va[4], sig.va[5], sig.va[6],
                     static_cast<unsigned long long>(key.hi),
                     static_cast<unsigned long long>(key.lo));
    }
}

void TryRememberSignature(const StageSig& sig, const Vulkan::Pipeline* pipeline) {
    g_attempts.fetch_add(1, std::memory_order_relaxed);
    if (pipeline == nullptr) {
        g_unmatched.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Key128 match{};
    u32 matches = 0;
    {
        std::scoped_lock lock{g_mutex};
        for (size_t i = 0; i < g_count; ++i) {
            const Entry& e = At(i);
            if (e.sig != sig) {
                continue;
            }
            if (matches == 0) {
                match = e.key;
            }
            ++matches;
        }
    }
    // The lock is released before the decision: Bind::Remember takes native_bind's own mutex, and
    // no code path takes that mutex and then this one (HashKey registers before any bind lock,
    // Lookup/Remember never call back into this module), so the order is fixed — but the ring must
    // not be held across a foreign lock.
    if (matches == 1) {
        Bind::Remember(match, pipeline);
        g_matched.fetch_add(1, std::memory_order_relaxed);
        if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr, "[native-fill] matched key=%016llx%016llx\n",
                         static_cast<unsigned long long>(match.hi),
                         static_cast<unsigned long long>(match.lo));
        }
    } else if (matches == 0) {
        g_unmatched.fetch_add(1, std::memory_order_relaxed);
        if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr,
                         "[native-fill] unmatched ps=%08x vs=%08x gs=%08x es=%08x hs=%08x ls=%08x\n",
                         sig.va[1], sig.va[2], sig.va[3], sig.va[4], sig.va[5], sig.va[6]);
        }
    } else {
        g_ambiguous.fetch_add(1, std::memory_order_relaxed);
        if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr,
                         "[native-fill] ambiguous ps=%08x vs=%08x gs=%08x es=%08x hs=%08x ls=%08x "
                         "keys=%u\n",
                         sig.va[1], sig.va[2], sig.va[3], sig.va[4], sig.va[5], sig.va[6], matches);
        }
    }
}

Stats StatsSnapshot() {
    Stats stats{};
    stats.registered = g_registered.load(std::memory_order_relaxed);
    stats.evicted = g_evicted.load(std::memory_order_relaxed);
    stats.attempts = g_attempts.load(std::memory_order_relaxed);
    stats.matched = g_matched.load(std::memory_order_relaxed);
    stats.ambiguous = g_ambiguous.load(std::memory_order_relaxed);
    stats.unmatched = g_unmatched.load(std::memory_order_relaxed);
    // Every attempt ends in exactly one of the three outcomes; a mismatch means an attempt path
    // forgot to count, which would make the counters useless for the divergence checker.
    if (stats.attempts != stats.matched + stats.ambiguous + stats.unmatched) {
        std::fprintf(stderr,
                     "[native-fill] counter inconsistency: attempts=%llu matched=%llu "
                     "ambiguous=%llu unmatched=%llu\n",
                     static_cast<unsigned long long>(stats.attempts),
                     static_cast<unsigned long long>(stats.matched),
                     static_cast<unsigned long long>(stats.ambiguous),
                     static_cast<unsigned long long>(stats.unmatched));
    }
    return stats;
}

} // namespace Native::Bind::Fill
