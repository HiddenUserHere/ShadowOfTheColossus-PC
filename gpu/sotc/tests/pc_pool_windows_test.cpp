// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-windows-test. Checks the windows of the native pool path (gpu/sotc/pc_pool_windows.cpp): window
// bookkeeping (address-ordered free list, alignment, coalescing, limits), the route of a call to its window, the park
// and reuse of a released block, the WindowFreshHost fallback, and the forced-fresh prediction of the model (pc_pool.cpp
// Predict with Request::force_fresh). It is a unit test over fakes: the game is not run.
//
// Build and run (scratch directory, not the repo):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_windows_test.cpp -o pc_pool_windows_test.exe
//   ./pc_pool_windows_test.exe   (prints PASS or FAIL; exit code 0 on PASS)

#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"
#include "../pc_pool_windows.cpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace SotcPcPool::Model;
namespace W = SotcPcPoolWindows;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        ++g_checks;                                                \
        if (!(cond)) {                                             \
            ++g_failures;                                          \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                          \
    } while (0)

constexpr uint64_t kMiB = W::kMiB;
constexpr uint64_t k64K = 0x10000;
constexpr uint64_t kBase = uint64_t{0x100} << 30;   // 1 MiB aligned, far below the limits
constexpr uint64_t kBaseB = uint64_t{0x200} << 30;  // a second window range
constexpr uint64_t kObjA = 0x80000000ull;           // the GPU arena object (window A's owner)
constexpr uint64_t kObjB = 0x80001000ull;           // the CPU arena object (window B's owner)
constexpr uint64_t kObjC = 0x80002000ull;           // the render-target allocator (window C's owner)

// A host that hands out consecutive VA from a base, aligned as asked; counts its calls.
class FakeHost final : public SotcPcPoolNativeFresh::FreshHost {
public:
    explicit FakeHost(uint64_t base) : next_(base) {}
    uint64_t ReserveVirtual(uint64_t size, uint64_t align) override {
        ++reserve_calls;
        if (align == 0 || size == 0) {
            return 0;
        }
        const uint64_t va = (next_ + align - 1) & ~(align - 1);
        next_ = va + size;
        return va;
    }
    int32_t BatchMap(const SotcPcPoolNativeFresh::BatchEntry*, size_t) override { return 0; }
    int reserve_calls = 0;

private:
    uint64_t next_;
};

// A host that returns an unaligned VA.
class MisalignedHost final : public SotcPcPoolNativeFresh::FreshHost {
public:
    uint64_t ReserveVirtual(uint64_t, uint64_t) override { return kBase + 4096; }
    int32_t BatchMap(const SotcPcPoolNativeFresh::BatchEntry*, size_t) override { return 0; }
};

// A host that gives no VA.
class NoVaHost final : public SotcPcPoolNativeFresh::FreshHost {
public:
    uint64_t ReserveVirtual(uint64_t, uint64_t) override { return 0; }
    int32_t BatchMap(const SotcPcPoolNativeFresh::BatchEntry*, size_t) override { return 0; }
};

// ---- Window ---------------------------------------------------------------------------------------------------

void TestWindowInit() {
    W::Window w;
    std::string why;
    CHECK(!w.Init(kBase + 4096, 128 * kMiB, &why));  // not 1 MiB aligned
    CHECK(!w.Init(kBase, 128 * kMiB + 4096, &why));  // size not a 1 MiB multiple
    CHECK(!w.Init(kBase, 0, &why));                  // empty
    CHECK(!w.Init(W::kOwnerTagLimit, kMiB, &why));   // starts at the owner-tag limit
    CHECK(!w.Init(W::kOwnerTagLimit - kMiB, 2 * kMiB, &why)); // ends above it
    CHECK(w.Init(W::kOwnerTagLimit - 128 * kMiB, 128 * kMiB, &why)); // ends exactly at the limit
    CHECK(w.free_bytes() == 128 * kMiB && w.free_runs() == 1);
    CHECK(W::kWindowLimit == W::kOwnerTagLimit && W::kOwnerTagLimit < W::kVaFieldLimit);
}

void TestWindowTakeGiveCoalesce() {
    W::Window w;
    CHECK(w.Init(kBase, 128 * kMiB, nullptr));
    CHECK(!w.CanTake(129 * kMiB, kMiB));
    CHECK(!w.CanTake(kMiB, 3)); // alignment not a power of two
    CHECK(w.CanTake(128 * kMiB, kMiB));
    CHECK(w.Take(129 * kMiB, kMiB) == 0);

    // a: 32 MiB at the base. small: a 64 KiB block at base+32 MiB. b: 32 MiB at the next 1 MiB boundary (base+33 MiB),
    // so the 64 KiB pad between small and b stays free.
    const uint64_t a = w.Take(32 * kMiB, kMiB);
    const uint64_t small = w.Take(k64K, k64K);
    const uint64_t b = w.Take(32 * kMiB, kMiB);
    CHECK(a == kBase);
    CHECK(small == kBase + 32 * kMiB);
    CHECK(b == kBase + 33 * kMiB);
    CHECK(w.live_blocks() == 3);
    CHECK(w.free_runs() == 2); // [base+32M+64K, base+33M) (the pad before b) and [base+65M, end)
    CHECK(w.free_bytes() == 128 * kMiB - 32 * kMiB - 32 * kMiB - k64K);

    // Give rejects what is not a live block of this size.
    CHECK(!w.Give(a, kMiB));
    CHECK(!w.Give(kBase + 100 * kMiB, kMiB));
    CHECK(w.IsLive(a, 32 * kMiB) && !w.IsLive(a, kMiB));

    // Give a: nothing free before it, so it becomes its own run.
    CHECK(w.Give(a, 32 * kMiB));
    CHECK(w.free_runs() == 3);
    CHECK(!w.Give(a, 32 * kMiB)); // no longer live

    // Give small: it joins the run after it and the run before it: [base, base+33M).
    CHECK(w.Give(small, k64K));
    CHECK(w.free_runs() == 2);
    CHECK(w.free_bytes() == 128 * kMiB - 32 * kMiB);

    // Give b: everything coalesces into one run.
    CHECK(w.Give(b, 32 * kMiB));
    CHECK(w.free_runs() == 1);
    CHECK(w.free_bytes() == 128 * kMiB && w.live_blocks() == 0);
}

void TestWindowFirstFitAddressOrder() {
    W::Window w;
    CHECK(w.Init(kBase, 64 * kMiB, nullptr));
    // Two holes: take the first block, free it, then take a block that does not fit the first hole.
    const uint64_t x = w.Take(8 * kMiB, kMiB);
    const uint64_t y = w.Take(8 * kMiB, kMiB);
    CHECK(x == kBase && y == kBase + 8 * kMiB);
    CHECK(w.Give(x, 8 * kMiB));
    // The lowest run that fits 16 MiB is the one after y (the hole at the base is only 8 MiB).
    const uint64_t z = w.Take(16 * kMiB, kMiB);
    CHECK(z == kBase + 16 * kMiB);
}

// ---- Table ----------------------------------------------------------------------------------------------------

// Reserves the three windows from one host; returns the number of successful reserves.
int ReserveAll(W::Table& t, FakeHost& host, std::vector<std::string>* names) {
    int ok = 0;
    for (int slot = 0; slot < W::kSlotCount; ++slot) {
        W::RegisterFn reg = [names](uint64_t, uint64_t, const char* name) {
            if (names != nullptr) {
                names->push_back(name);
            }
            return true;
        };
        std::string why;
        ok += t.Reserve(slot, host, reg, &why) ? 1 : 0;
    }
    return ok;
}

void TestReserve() {
    {
        W::Table t;
        FakeHost host(kBase);
        std::vector<std::string> names;
        CHECK(ReserveAll(t, host, &names) == 3);
        CHECK(names.size() == 3 && names[0] == "A gpu-arena" && names[1] == "B mask-heap");
        CHECK(t.window(0).size() == 128 * kMiB && t.window(1).size() == 16 * kMiB);
        CHECK(t.window(2).size() == 352 * kMiB);
        CHECK(t.window(0).base() % kMiB == 0);
        CHECK(t.stats().reserve_ok == 3);
        // A second reserve of an active window is a no-op.
        std::string why;
        CHECK(t.Reserve(0, host, nullptr, &why));
        CHECK(host.reserve_calls == 3);
    }
    {
        // A failed registration keeps the window active (the announce path covers its data).
        W::Table t;
        FakeHost host(kBase);
        W::RegisterFn refuse = [](uint64_t, uint64_t, const char*) { return false; };
        std::string why;
        CHECK(t.Reserve(0, host, refuse, &why));
        CHECK(t.window(0).active() && t.stats().register_failed == 1);
    }
    {
        // A host with no VA, and an unaligned VA, leave the window inactive (counted).
        W::Table t;
        NoVaHost none;
        MisalignedHost bad;
        std::string why;
        CHECK(!t.Reserve(0, none, nullptr, &why));
        CHECK(!t.Reserve(1, bad, nullptr, &why));
        CHECK(!t.window(0).active() && !t.window(1).active());
        CHECK(t.stats().reserve_failed == 2);
    }
    {
        // A range that ends above the owner-tag limit is refused (its VA leaked at boot, counted).
        W::Table t;
        FakeHost high(W::kOwnerTagLimit - 64 * kMiB);
        std::string why;
        CHECK(!t.Reserve(2, high, nullptr, &why)); // 352 MiB from 64 MiB below the limit
        CHECK(!t.window(2).active() && t.stats().reserve_limit == 1);
    }
}

void TestRouteAndBind() {
    W::Table t;
    FakeHost host(kBase);
    CHECK(ReserveAll(t, host, nullptr) == 3);
    // Unbound windows route nothing.
    CHECK(t.Route(kObjA, 2, W::kAnyKey, 32 * kMiB, k64K) == -1);
    CHECK(t.Bind(0, kObjA, W::kAnyKey));
    CHECK(t.Bind(1, kObjB, W::kMaskKey));
    CHECK(!t.Bind(0, 0, W::kAnyKey)); // a null owner never binds
    CHECK(t.Route(kObjA, 2, W::kAnyKey, 32 * kMiB, k64K) == 0);
    CHECK(t.Route(kObjA, 2, 99, 32 * kMiB, k64K) == 0);      // any key
    CHECK(t.Route(kObjA, 2, W::kAnyKey, 16 * kMiB, k64K) == -1); // not the block size
    CHECK(t.Route(kObjA, 1, W::kAnyKey, 32 * kMiB, k64K) == -1); // not the kind
    CHECK(t.Route(kObjB, 1, 16, 8 * kMiB, k64K) == -1);      // another memtype
    CHECK(t.Route(kObjB, 1, W::kMaskKey, 8 * kMiB, k64K) == 1);
    CHECK(t.Route(kObjC, 2, W::kAnyKey, 0x14280000, k64K) == -1); // C not bound
    CHECK(t.stats().routed[0] == 2 && t.stats().routed[1] == 1);
}

void TestFullWindowFallsBack() {
    W::Table t;
    FakeHost host(kBase);
    CHECK(ReserveAll(t, host, nullptr) == 3);
    CHECK(t.Bind(0, kObjA, W::kAnyKey));
    // Four 32 MiB blocks fill window A (128 MiB). The fifth routes to the normal path and is counted as full.
    for (int i = 0; i < 4; ++i) {
        CHECK(t.Route(kObjA, 2, W::kAnyKey, 32 * kMiB, k64K) == 0);
        CHECK(t.TakeVa(0, 32 * kMiB, kMiB) != 0);
    }
    CHECK(t.window(0).live_blocks() == 4 && t.window(0).free_bytes() == 0);
    CHECK(t.Route(kObjA, 2, W::kAnyKey, 32 * kMiB, k64K) == -1);
    CHECK(t.stats().full[0] == 1);
    CHECK(t.stats().served[0] == 4);
    // TakeVa on a full window counts a failed take and returns 0.
    CHECK(t.TakeVa(0, 32 * kMiB, kMiB) == 0);
    CHECK(t.stats().take_failed[0] == 1);
}

void TestCommitReleaseReuse() {
    W::Table t;
    FakeHost host(kBase);
    CHECK(ReserveAll(t, host, nullptr) == 3);
    CHECK(t.Bind(0, kObjA, W::kAnyKey));
    const uint64_t va1 = t.TakeVa(0, 32 * kMiB, kMiB);
    const uint64_t va2 = t.TakeVa(0, 32 * kMiB, kMiB);
    CHECK(va1 != 0 && va2 != 0);
    const uint64_t n1 = 0x90000000ull;
    const uint64_t n2 = 0x90001000ull;
    t.Commit(0, va1, 32 * kMiB, n1);
    t.Commit(0, va2, 32 * kMiB, n2);
    CHECK(t.stats().committed[0] == 2);

    // A release with the wrong node is not a park; a release outside every window is an ordinary free (not counted).
    CHECK(!t.Release(n2, va1, 32 * kMiB));
    CHECK(t.stats().park_refused[0] == 1);
    CHECK(!t.Release(n1, 0x123000, 32 * kMiB));
    CHECK(t.stats().park_refused[0] == 1);

    // Park n2 first (the higher VA), then n1. Reuse returns the lowest VA first.
    CHECK(t.Release(n2, va2, 32 * kMiB));
    CHECK(t.Release(n1, va1, 32 * kMiB));
    CHECK(t.parked(0).size() == 2);
    CHECK(!t.Release(n1, va1, 32 * kMiB)); // already parked
    CHECK(t.stats().parked[0] == 2 && t.stats().park_refused[0] == 2);

    uint64_t node = 0;
    CHECK(!t.Reuse(kObjB, W::kAnyKey, 32 * kMiB, &node)); // another owner
    CHECK(!t.Reuse(kObjA, W::kAnyKey, 8 * kMiB, &node));  // another size: nothing parked of it
    CHECK(t.Reuse(kObjA, W::kAnyKey, 32 * kMiB, &node) && node == n1);
    CHECK(t.Reuse(kObjA, W::kAnyKey, 32 * kMiB, &node) && node == n2);
    CHECK(!t.Reuse(kObjA, W::kAnyKey, 32 * kMiB, &node));
    CHECK(t.stats().reused[0] == 2 && t.stats().reuse_miss[0] >= 1);
    // A reused block is live and bound again: a parked block that is released again parks again.
    CHECK(t.window(0).live_blocks() == 2);
    CHECK(t.Release(n1, va1, 32 * kMiB));
}

void TestGiveVa() {
    W::Table t;
    FakeHost host(kBase);
    CHECK(ReserveAll(t, host, nullptr) == 3);
    CHECK(t.Bind(0, kObjA, W::kAnyKey));
    const uint64_t va = t.TakeVa(0, 32 * kMiB, kMiB);
    CHECK(va != 0 && t.window(0).free_bytes() == 96 * kMiB);
    t.GiveVa(0, va, 32 * kMiB);
    CHECK(t.stats().given_back[0] == 1 && t.window(0).free_bytes() == 128 * kMiB);
    t.GiveVa(0, va, 32 * kMiB); // not live any more
    CHECK(t.stats().take_failed[0] == 1);
}

// ---- import and drop of committed blocks -------------------------------------------------------------------------

void TestImportDrop() {
    std::vector<uint64_t> imports; // every block the host was asked to import (refused ones too)
    std::vector<uint64_t> drops;
    bool refuse = false;
    W::Table t([&](uint64_t va, uint64_t) {
        imports.push_back(va);
        return !refuse;
    }, [&](uint64_t va, uint64_t) { drops.push_back(va); });
    FakeHost host(kBase);
    CHECK(ReserveAll(t, host, nullptr) == 3);
    CHECK(t.Bind(0, kObjA, W::kAnyKey));
    const uint64_t va1 = t.TakeVa(0, 32 * kMiB, kMiB);
    const uint64_t va2 = t.TakeVa(0, 32 * kMiB, kMiB);
    const uint64_t va3 = t.TakeVa(0, 32 * kMiB, kMiB);
    const uint64_t n1 = 0x90000000ull;
    const uint64_t n2 = 0x90001000ull;
    const uint64_t n3 = 0x90002000ull;
    CHECK(imports.empty()); // nothing is imported before the commit

    // A commit imports the block (the writer has backed it).
    t.Commit(0, va1, 32 * kMiB, n1);
    CHECK(imports.size() == 1 && imports[0] == va1);
    CHECK(t.imported(0).count(va1) == 1 && t.stats().imported[0] == 1);

    // A refused import is counted: the block stays committed and bound, and is not imported.
    refuse = true;
    t.Commit(0, va2, 32 * kMiB, n2);
    CHECK(imports.size() == 2 && t.stats().import_refused[0] == 1);
    CHECK(t.imported(0).count(va2) == 0 && t.stats().committed[0] == 2);

    // An ordinary free of a block that is not imported drops nothing.
    t.Forget(va2);
    CHECK(drops.empty() && t.stats().dropped[0] == 0);

    // A parked block keeps its import: the park is not an unmap, and a second Forget of a parked block drops nothing.
    CHECK(t.Release(n1, va1, 32 * kMiB));
    t.Forget(va1);
    CHECK(drops.empty() && t.imported(0).count(va1) == 1);

    // An ordinary free of an imported block (not parked) drops its import; the window keeps its VA live.
    refuse = false;
    t.Commit(0, va3, 32 * kMiB, n3);
    CHECK(imports.size() == 3 && t.imported(0).count(va3) == 1 && t.stats().imported[0] == 2);
    t.Forget(va3);
    CHECK(drops.size() == 1 && drops[0] == va3 && t.stats().dropped[0] == 1);
    CHECK(t.imported(0).count(va3) == 0 && t.imported(0).count(va1) == 1);
    t.Forget(va3); // already dropped: nothing more
    CHECK(drops.size() == 1 && t.stats().dropped[0] == 1);
    CHECK(t.window(0).live_blocks() == 3); // the VA stays live in the window (the pool may take it back)
    t.Forget(0x123000); // outside every window: nothing of the windows

    // A table without the host callbacks commits without importing (the default constructor).
    W::Table plain;
    FakeHost host2(kBaseB);
    CHECK(ReserveAll(plain, host2, nullptr) == 3);
    CHECK(plain.Bind(0, kObjA, W::kAnyKey));
    const uint64_t pva = plain.TakeVa(0, 32 * kMiB, kMiB);
    plain.Commit(0, pva, 32 * kMiB, n1);
    CHECK(plain.stats().committed[0] == 1 && plain.stats().imported[0] == 0 && plain.stats().import_refused[0] == 0);
}

// ---- WindowFreshHost -------------------------------------------------------------------------------------------

void TestWindowFreshHost() {
    W::Table t;
    FakeHost reserve(kBase);
    CHECK(ReserveAll(t, reserve, nullptr) == 3);
    CHECK(t.Bind(0, kObjA, W::kAnyKey));
    FakeHost normal(kBaseB);
    W::WindowFreshHost wf(normal, t);

    // Selected: the VA is the window's, and the normal host is not called.
    wf.Select(0);
    const uint64_t va = wf.ReserveVirtual(32 * kMiB, kMiB);
    CHECK(va >= t.window(0).base() && va + 32 * kMiB <= t.window(0).end());
    CHECK(wf.last_va() == va && wf.last_bytes() == 32 * kMiB);
    CHECK(normal.reserve_calls == 0);

    // Not selected: the normal host gives the VA and last_va is 0 (nothing to commit or give back).
    wf.Select(-1);
    const uint64_t other = wf.ReserveVirtual(32 * kMiB, kMiB);
    CHECK(other != 0 && normal.reserve_calls == 1 && wf.last_va() == 0);

    // Selected and full: the normal host is the fallback, and the table counts the failed take.
    wf.Select(0);
    for (int i = 0; i < 3; ++i) {
        CHECK(wf.ReserveVirtual(32 * kMiB, kMiB) != 0);
    }
    const uint64_t fallback = wf.ReserveVirtual(32 * kMiB, kMiB);
    CHECK(fallback != 0 && normal.reserve_calls == 2 && wf.last_va() == 0);
    CHECK(t.stats().take_failed[0] == 1);

    // The batch flush always goes to the normal host.
    CHECK(wf.BatchMap(nullptr, 0) == 0);

    // A refused call gives its window block back: the give-back restores the free bytes.
    wf.Select(0);
    t.GiveVa(0, va, 32 * kMiB);
    CHECK(t.window(0).free_bytes() == 32 * kMiB); // one freed block of the four taken earlier is free again
}

// ---- Predict with force_fresh --------------------------------------------------------------------------------

Set MakeSet(int id) {
    Set s;
    s.id = id;
    s.has_bitmap = id != 0;
    return s;
}

// S2 (the GPU primary set) has one free-VA run of 1 MiB with 64 KiB backed, two cached pages, and bitmap bits 0-1.
Pools MakePools() {
    Pools p;
    for (int i = 0; i < kSetCount; ++i) {
        p.set[i] = MakeSet(i);
    }
    Set& s2 = p.set[2];
    s2.free_va_pages = 16;
    s2.cached_phys = 2;
    s2.stored_free_count = 1;
    s2.page_cap = 64;
    s2.words = {0x3};
    s2.words_addr = 0x1234;
    s2.free_va.push_back(Run{uint64_t{0x40000000}, kMiB, k64K, 0x50000000});
    return p;
}

void TestForcedFreshPredict() {
    const Pools pre = MakePools();
    Request req;
    req.kind = 2;
    req.size = k64K;
    req.align = k64K;

    // Without the window route the free run is the exact hit (its VA is known).
    Pools post;
    const Prediction exact = Predict(pre, req, nullptr, &post);
    CHECK(exact.outcome == Outcome::Predicted && exact.path == Path::PrimaryHitExact);
    CHECK(exact.va == 0x40000000 && !exact.fresh);

    // With the route the same call is a fresh block: no free-VA run is used, its VA is the window's (not predicted).
    req.force_fresh = true;
    Pools post2;
    const Prediction fresh = Predict(pre, req, nullptr, &post2);
    CHECK(fresh.outcome == Outcome::Predicted && fresh.path == Path::FreshPlain);
    CHECK(fresh.fresh && fresh.va == 0 && fresh.va_align == kMiB && fresh.size == k64K);
    CHECK(fresh.runs.size() == 1 && fresh.runs[0].set == 2 && fresh.runs[0].pages == 1);
    // The free-VA run is untouched by the fresh path, and the bitmap bit is taken.
    CHECK(post2.set[2].free_va.size() == 1 && post2.set[2].free_va[0].size == kMiB);
    CHECK(post2.set[2].words[0] == 0x2);
    CHECK(post2.set[2].cached_phys == 1);

    // The route's eligibility: the default mask does not take a fresh path, the fresh mask does.
    CHECK(!Eligible(fresh, kDefaultPathMask));
    CHECK(Eligible(fresh, kDefaultPathMask | kFreshPathMask));
    CHECK(Eligible(exact, kDefaultPathMask));
}

} // namespace

int main() {
    TestWindowInit();
    TestWindowTakeGiveCoalesce();
    TestWindowFirstFitAddressOrder();
    TestReserve();
    TestRouteAndBind();
    TestFullWindowFallsBack();
    TestCommitReleaseReuse();
    TestGiveVa();
    TestImportDrop();
    TestWindowFreshHost();
    TestForcedFreshPredict();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
