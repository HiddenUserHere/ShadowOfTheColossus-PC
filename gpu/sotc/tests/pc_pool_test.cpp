// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-test. Standalone check of the PcPool verification model (gpu/sotc/pc_pool.cpp, the
// Model namespace; the port glue is compiled out). Hand-computed cases follow the disassembly of
// pool_alloc_region (0x19e00), pool_take_free_run (0x1a800), vmem_query_reserve_map (0x1abf0) and
// pool_free_block (0x1b0e0), including the regression cases logged in-game (see the comments on each
// test). Then randomized allocations check the model's invariants. It does not prove the model matches
// the game: that is what the in-game comparison measures.
//
// Build and run (scratch directory, not the repo):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_test.cpp -o pc_pool_test.exe
//   ./pc_pool_test.exe   (prints PASS or FAIL; exit code 0 on PASS)

#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

namespace {

using namespace SotcPcPool::Model;

int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            ++g_failures;                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);    \
        }                                                          \
    } while (0)

constexpr uint64_t kP = kPageBytes;
constexpr uint64_t kOneMiB = kMiB;

Run R(uint64_t base, uint64_t size, uint64_t backed) { return Run{base, size, backed}; }

// words: the bitmap (bit = phys page index); cap: the index bound (+0x60), 0 means "all bits of words".
Set MakeSet(int id, std::vector<Run> runs, std::vector<uint64_t> words, int32_t cached) {
    Set s;
    s.id = id;
    s.has_bitmap = id != 0;
    s.free_va = std::move(runs);
    s.words = std::move(words);
    s.cached_phys = cached;
    s.page_cap = s.has_bitmap ? static_cast<uint32_t>(s.words.size() * 64) : 0;
    uint64_t pages = 0;
    for (const Run& r : s.free_va) {
        pages += r.size / kP;
    }
    s.free_va_pages = static_cast<int32_t>(pages);
    s.stored_free_count = static_cast<int32_t>(s.free_va.size());
    return s;
}

Pools MakePools(Set s0, Set s1, Set s2) {
    Pools p;
    p.set[0] = std::move(s0);
    p.set[1] = std::move(s1);
    p.set[2] = std::move(s2);
    return p;
}

Set Empty(int id) { return MakeSet(id, {}, id ? std::vector<uint64_t>{0} : std::vector<uint64_t>{}, 0); }

Request Req(int32_t kind, uint64_t pages, uint64_t align = kP) {
    Request r;
    r.kind = kind;
    r.size = pages * kP;
    r.align = align;
    r.mode = 0;
    return r;
}

// The state the game should leave for a predicted block, with the observed VA (for fresh blocks).
Observed ObservedFrom(const Prediction& p, const Pools& post, uint64_t va) {
    Observed o;
    o.node = p.outcome == Outcome::Null ? 0 : 0x1000;
    o.va = va != 0 ? va : p.va;
    o.size = p.size;
    o.slot = p.slot;
    o.post = post;
    return o;
}

bool SameList(const std::vector<Run>& a, const std::vector<Run>& b) {
    return a == b;
}

bool SamePools(const Pools& a, const Pools& b) {
    for (int i = 0; i < kSetCount; ++i) {
        if (!SameSet(a.set[i], b.set[i])) {
            return false;
        }
    }
    return true;
}

// Flexible kind-1 hit: first fit, 64 KiB alignment, the free run is split (its backed bytes exceed the request).
void TestFlexHit() {
    Pools pre = MakePools(MakeSet(0, {R(0x10000000, 16 * kP, 16 * kP)}, {}, 0), Empty(1), Empty(2));
    Pools post;
    Prediction p = Predict(pre, Req(1, 4), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.slot == 0);
    CHECK(p.va == 0x10000000);
    CHECK(p.size == 4 * kP);
    CHECK(!p.fresh);
    CHECK(p.runs.empty());
    CHECK(SameList(post.set[0].free_va, {R(0x10000000 + 4 * kP, 12 * kP, 12 * kP)}));
    CHECK(post.set[0].free_va_pages == 12);
    CHECK(post.set[0].region_count == 1);
    CHECK(post.set[0].alloc_bytes == 4 * kP);
    CHECK(post.set[0].backed_bytes == 4 * kP);
    CHECK(post.set[0].stored_free_count == 1);
}

// Disassembly-derived tail drop (0x1a800 sets the node size to the request outside the split branch):
// a remainder whose backed bytes do not exceed the request is not kept. Documented, verified only in game.
void TestFlexTailDropWhenNotBacked() {
    Pools pre = MakePools(MakeSet(0, {R(0x10000000, 16 * kP, 0)}, {}, 0), Empty(1), Empty(2));
    Pools post;
    Prediction p = Predict(pre, Req(1, 4), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(post.set[0].free_va.empty());
    CHECK(post.set[0].free_va_pages == 12); // the counter still drops by the request, the list does not hold the rest
    CHECK(post.set[0].stored_free_count == 0);
}

// In-game log, kind 1, size 0x1b0000, align 0x10, va 0x1000b00000 in S1: the game took the block from the
// free run with no phys work (cached 128 unchanged, bitmap unchanged). The model used to clear bits.
void TestOnionHitKeepsBitmap() {
    const uint64_t base = 0x1000b00000ull;
    Pools pre = MakePools(Empty(0), MakeSet(1, {R(base, 0x2000000, 0x2000000)}, {~uint64_t{0}, ~uint64_t{0}}, 128),
                          Empty(2));
    Pools post;
    Request r = Req(1, 0x1b0000 / kP, 0x10);
    Prediction p = Predict(pre, r, nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.slot == 1);
    CHECK(p.va == base);
    CHECK(p.size == 0x1b0000);
    CHECK(p.runs.empty());
    CHECK(post.set[1].cached_phys == 128);
    CHECK(post.set[1].words == pre.set[1].words);
    CHECK(post.set[1].free_va_pages == pre.set[1].free_va_pages - 27);
}

// In-game log, kind 1, size 0x200000, align 0x10: the game returned 0x1000d00000 from the free run that starts
// at 0x1000cb0000. The search aligns to 1 MiB, the 0x50000 pad stays free as its own run.
void TestOneMiBAlignmentKeepsPad() {
    const uint64_t base = 0x1000cb0000ull;
    const uint64_t size = 0x1000000;
    Pools pre = MakePools(Empty(0), MakeSet(1, {R(base, size, size)}, {~uint64_t{0}}, 0), Empty(2));
    Pools post;
    Prediction p = Predict(pre, Req(1, 0x200000 / kP, 0x10), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.va == 0x1000d00000ull);
    CHECK(SameList(post.set[1].free_va, {R(base, 0x50000, 0x50000),
                                         R(0x1000f00000ull, size - 0x50000 - 0x200000, size - 0x50000 - 0x200000)}));
    CHECK(post.set[1].free_va_pages == static_cast<int32_t>((size - 0x200000) / kP));
}

// The spill order: kind 2 prefers Garlic (2) for its VA; the page loop uses the bitmap of S2 when it has
// cached pages (no spill). Fresh VA, so the block's VA is not predicted.
void TestGarlicFreshUsesOwnBitmap() {
    Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {}, {0xF}, 4));
    Pools post;
    Prediction p = Predict(pre, Req(2, 2), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.fresh);
    CHECK(p.slot == 2);
    CHECK(p.va_align == kOneMiB);
    CHECK(p.runs.size() == 1 && p.runs[0].set == 2 && p.runs[0].first_page == 0 && p.runs[0].pages == 2);
    CHECK(post.set[2].words[0] == 0xC);
    CHECK(post.set[2].cached_phys == 2);
    CHECK(post.set[2].region_count == 1);
    CHECK(post.set[2].alloc_bytes == 2 * kP);
    CHECK(post.set[2].backed_bytes == 2 * kP);
    CHECK(post.set[1].cached_phys == 0);
}

// The primary (Garlic) has no cached pages and no free VA: the page loop continues in the spill set Onion,
// whose bits are taken with the protect op type 3 (spec 2.2 step 5c).
void TestSpillToOnion() {
    Pools pre = MakePools(Empty(0), MakeSet(1, {}, {0x7}, 3), Empty(2));
    Pools post;
    Prediction p = Predict(pre, Req(2, 2), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.fresh);
    CHECK(p.slot == 2);
    CHECK(p.runs.size() == 1 && p.runs[0].set == 1 && p.runs[0].first_page == 0 && p.runs[0].pages == 2);
    CHECK(p.runs[0].protect_type == 3);
    CHECK(post.set[1].words[0] == 0x4);
    CHECK(post.set[1].cached_phys == 1);
    CHECK(post.set[1].backed_bytes == 2 * kP);
    CHECK(post.set[2].backed_bytes == 0);
    CHECK(post.set[2].region_count == 1);
}

// Kind 1 with the Garlic set as spill: the protect type is 0 (F.id == 2).
void TestSpillToGarlicProtectType() {
    Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {}, {0x3}, 2));
    Pools post;
    Prediction p = Predict(pre, Req(1, 1), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.slot == 1);
    // The primary S1 is empty, so the page loop spills to S2: op 4 type 0 (F.id == 2, local_4c = 0).
    CHECK(p.runs.size() == 1 && p.runs[0].set == 2 && p.runs[0].protect_type == 0);
}

// The primary-set search runs before the fresh path, so a fitting free run of the other set is not used for
// the VA: kind 2 with a free run in S1 only takes a fresh VA and the bits of S2.
void TestFallbackVaNotUsed() {
    Pools pre = MakePools(Empty(0), MakeSet(1, {R(0x20000000, 8 * kP, 8 * kP)}, {0xFF}, 8),
                          MakeSet(2, {}, {0x3}, 2));
    Pools post;
    Prediction p = Predict(pre, Req(2, 2), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.fresh);
    CHECK(p.slot == 2);
    CHECK(post.set[1].free_va.size() == 1);
    CHECK(post.set[1].free_va[0] == R(0x20000000, 8 * kP, 8 * kP));
}

void TestEarlyReject() {
    Pools pre = MakePools(Empty(0), Empty(1), Empty(2));
    Pools post;
    Prediction p = Predict(pre, Req(2, 4), nullptr, &post);
    CHECK(p.outcome == Outcome::Null);
    CHECK(SamePools(pre, post));
}

// Reserve-map (vmem_query_reserve_map flag 1, 0x1abf0) with the cached count at zero: the mapped pages of the
// free node come back to the bitmap, the range is unmapped and consumed, and the page loop continues.
void TestReserveMapWithMap() {
    Pools pre = MakePools(Empty(0), MakeSet(1, {}, {0x7}, 3), MakeSet(2, {R(0x30000000, kP, kP)}, {0}, 0));
    Pools post;
    PhysMap map;
    map.Map(0x30000000, kP, 5);
    Prediction p = Predict(pre, Req(2, 2), &map, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.fresh);
    CHECK(p.unmapped.size() == 1 && p.unmapped[0].base == 0x30000000 && p.unmapped[0].size == kP);
    // Runs: first the page freed by the reserve-map (S2 bit 5), then the spill set S1 (bit 0).
    CHECK(p.runs.size() == 2);
    if (p.runs.size() == 2) {
        CHECK(p.runs[0].set == 2 && p.runs[0].first_page == 5 && p.runs[0].pages == 1);
        CHECK(p.runs[1].set == 1 && p.runs[1].first_page == 0 && p.runs[1].pages == 1 && p.runs[1].protect_type == 3);
    }
    CHECK(post.set[2].free_va.empty());
    CHECK(post.set[2].stored_free_count == 0);
    CHECK(post.set[2].free_va_pages == 0);
    CHECK(post.set[2].cached_phys == 0);
    CHECK(post.set[2].words[0] == 0);
    CHECK(post.set[1].cached_phys == 2);
    CHECK(post.set[1].words[0] == 0x6);
}

void TestReserveMapNeedsMap() {
    Pools pre = MakePools(Empty(0), MakeSet(1, {}, {0x7}, 3), MakeSet(2, {R(0x30000000, kP, kP)}, {0}, 0));
    Pools post;
    Prediction p = Predict(pre, Req(2, 2), nullptr, &post);
    CHECK(p.outcome == Outcome::Unmodelled);
}

// POOL_FIX (spec 2.5): the bitmap has no free bit though the cached count says 3. The patch zeroes the cached
// count and takes the VA-node path; with no free-VA node left (and no spill VA) the game dereferences null.
void TestWouldCrashWhenNoNodeLeft() {
    Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {R(0x30000000, 3 * kP, 0)}, {0}, 3));
    PhysMap map;
    Pools post;
    Prediction p = Predict(pre, Req(2, 4), &map, &post);
    CHECK(p.outcome == Outcome::WouldCrash);
    Prediction q = Predict(pre, Req(2, 4), nullptr, &post);
    CHECK(q.outcome == Outcome::Unmodelled);
}

void TestCompare() {
    Pools pre = MakePools(Empty(0), MakeSet(1, {R(0x20000000, 8 * kP, 8 * kP)}, {0xF3}, 6), Empty(2));
    Pools post;
    Prediction p = Predict(pre, Req(1, 6), nullptr, &post);
    CHECK(p.outcome == Outcome::Predicted);

    int outside = 0;
    Observed good = ObservedFrom(p, post, 0);
    CHECK(Compare(p, pre, post, good, nullptr, &outside) == 0);
    CHECK(outside == 0);

    Observed bad_va = good;
    bad_va.va += kP;
    CHECK(Compare(p, pre, post, bad_va, nullptr, &outside) == kFieldVa);

    Observed bad_bits = good;
    bad_bits.post.set[1].words[0] |= 4; // bit 2 is clear in the fixture (0xF3)
    CHECK(Compare(p, pre, post, bad_bits, nullptr, &outside) == kFieldBitmap);

    Observed bad_count = good;
    bad_count.post.set[1].cached_phys += 1;
    CHECK(Compare(p, pre, post, bad_count, nullptr, &outside) == kFieldCached);

    Observed no_node = good;
    no_node.node = 0;
    CHECK((Compare(p, pre, post, no_node, nullptr, &outside) & kFieldNode) != 0);

    // A change in a set the prediction does not touch is counted outside, not as a mismatch.
    Observed nested = good;
    nested.post.set[0].region_count += 1;
    outside = 0;
    CHECK(Compare(p, pre, post, nested, nullptr, &outside) == 0);
    CHECK(outside == 1);

    // Null prediction: the game must return 0 and change nothing.
    Pools empty = MakePools(Empty(0), Empty(1), Empty(2));
    Pools post2;
    Prediction n = Predict(empty, Req(2, 4), nullptr, &post2);
    CHECK(n.outcome == Outcome::Null);
    Observed nul;
    nul.node = 0;
    nul.post = empty;
    CHECK(Compare(n, empty, post2, nul, nullptr, &outside) == 0);
    nul.node = 0x1000;
    CHECK((Compare(n, empty, post2, nul, nullptr, &outside) & kFieldNode) != 0);
}

// Fresh blocks: the VA is only known from the game, and must be 1 MiB aligned.
void TestCompareFresh() {
    Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {}, {0xF}, 4));
    Pools post;
    Prediction p = Predict(pre, Req(2, 2), nullptr, &post);
    CHECK(p.fresh);
    int outside = 0;
    CHECK(Compare(p, pre, post, ObservedFrom(p, post, 0x40000000), nullptr, &outside) == 0);
    CHECK((Compare(p, pre, post, ObservedFrom(p, post, 0x40010000), nullptr, &outside) & kFieldVa) != 0);
}

void TestFreeAccounting() {
    FreeAcc acc[kSetCount] = {};
    // Onion/Garlic block (backed 4 pages): region -1, alloc and backed -4 pages; the VA is consumed (net 0),
    // the 4 mapped pages come back to the bitmap and to +0x74.
    AddFree(acc, 1, 4 * kP, 4 * kP);
    CHECK(acc[1].frees == 1);
    CHECK(acc[1].region == -1);
    CHECK(acc[1].alloc == -static_cast<int64_t>(4 * kP));
    CHECK(acc[1].backed == -static_cast<int64_t>(4 * kP));
    CHECK(acc[1].free_va_pages == 0);
    CHECK(acc[1].bits == 4);
    CHECK(acc[1].cached == 4);
    // Flexible block, fully backed: back on the address-ordered list.
    AddFree(acc, 0, 2 * kP, 2 * kP);
    CHECK(acc[0].free_va_pages == 2);
    CHECK(acc[0].bits == 0);

    Set before = MakeSet(1, {}, {0x0}, 0);
    before.region_count = 5;
    before.alloc_bytes = 8 * kP;
    before.backed_bytes = 8 * kP;
    Set after = before;
    after.region_count = 4;
    after.alloc_bytes = 4 * kP;
    after.backed_bytes = 4 * kP;
    after.words = {0xF};
    after.cached_phys = 4;
    int64_t va_delta = -1;
    CHECK(CheckFree(before, after, acc[1], &va_delta, nullptr) == 0);
    CHECK(va_delta == 0);

    Set wrong = after;
    wrong.region_count = 5;
    CHECK((CheckFree(before, wrong, acc[1], &va_delta, nullptr) & kFieldRegion) != 0);

    // The VA leaving the list would show as a change of +0x70: the model expects none (net 0 for Onion).
    Set returned = after;
    returned.free_va_pages += 4;
    CHECK((CheckFree(before, returned, acc[1], &va_delta, nullptr) & kFieldFreeVaPages) != 0);
}

// Randomized run: allocations against random pool states. Checks the model's invariants after each
// predicted allocation: the free runs stay sorted and disjoint; the counters move by the expected amounts;
// a free-run hit leaves the bitmap alone; a fresh block takes exactly the pages of its phys runs.
void TestRandomInvariants() {
    std::mt19937_64 rng(0x5017c0ffee);
    auto rnd = [&](uint64_t n) { return n == 0 ? 0 : rng() % n; };
    uint64_t predicted = 0, fresh = 0, hits = 0, nulls = 0, crashes = 0, unmodelled = 0;
    int violations = 0;

    for (int round = 0; round < 300; ++round) {
        Pools pool;
        for (int id = 0; id < kSetCount; ++id) {
            std::vector<Run> runs;
            uint64_t cursor = 0x10000000ull * static_cast<uint64_t>(id + 1);
            const int n = static_cast<int>(rnd(6));
            for (int i = 0; i < n; ++i) {
                cursor += rnd(4) * kP;
                const uint64_t size = (1 + rnd(20)) * kP;
                const uint64_t backed = rnd(2) ? size : 0;
                runs.push_back(Run{cursor, size, backed});
                cursor += size;
            }
            std::vector<uint64_t> words;
            int32_t cached = 0;
            if (id != 0) {
                words.resize(4);
                for (auto& w : words) {
                    for (int b = 0; b < 64; ++b) {
                        if (rnd(10) < 7) {
                            w |= uint64_t{1} << b;
                        }
                    }
                    cached += __builtin_popcountll(w);
                }
            }
            pool.set[id] = MakeSet(id, runs, words, cached);
        }
        for (int step = 0; step < 200; ++step) {
            const int32_t kind = rnd(2) ? 1 : 2;
            const uint64_t pages = 1 + rnd(12);
            const uint64_t align = std::vector<uint64_t>{kP, 0x40000, 0x100000}[rnd(3)];
            Pools post;
            Prediction p = Predict(pool, Req(kind, pages, align), nullptr, &post);
            switch (p.outcome) {
            case Outcome::Predicted: {
                ++predicted;
                // Sorted, disjoint free runs with no zero-size run.
                for (int s = 0; s < kSetCount; ++s) {
                    const auto& list = post.set[s].free_va;
                    for (size_t i = 0; i < list.size(); ++i) {
                        if (list[i].size == 0 || list[i].backed > list[i].size) {
                            ++violations;
                        }
                        if (i + 1 < list.size() && list[i].base + list[i].size > list[i + 1].base) {
                            ++violations;
                        }
                    }
                }
                // Counters: only the chosen set's region, alloc and backed move (plus the phys run pages).
                for (int s = 0; s < kSetCount; ++s) {
                    int64_t covered = 0;
                    for (const PhysRun& r : p.runs) {
                        if (r.set == s) {
                            covered += static_cast<int64_t>(r.pages);
                        }
                    }
                    const Set& a = pool.set[s];
                    const Set& b = post.set[s];
                    if (s == p.slot) {
                        if (b.region_count != a.region_count + 1) {
                            ++violations;
                        }
                    } else if (b.region_count != a.region_count) {
                        ++violations;
                    }
                    if (b.cached_phys != a.cached_phys - static_cast<int32_t>(covered)) {
                        ++violations;
                    }
                    if (static_cast<int64_t>(b.backed_bytes) - static_cast<int64_t>(a.backed_bytes) !=
                        covered * static_cast<int64_t>(kP) +
                            (s == p.slot && !p.fresh ? static_cast<int64_t>(p.size) : 0)) {
                        ++violations;
                    }
                    if (s != p.slot && s != 0 && b.words.size() == a.words.size()) {
                        // untouched bitmaps: only the phys runs clear bits
                        if (PopCount(b) != PopCount(a) - static_cast<uint64_t>(covered)) {
                            ++violations;
                        }
                    }
                }
                if (p.fresh) {
                    ++fresh;
                    uint64_t covered = 0;
                    for (const PhysRun& r : p.runs) {
                        covered += r.pages;
                    }
                    if (covered != pages) {
                        ++violations;
                    }
                } else {
                    ++hits;
                    const Set& a = pool.set[p.slot];
                    const Set& b = post.set[p.slot];
                    if (p.runs.size() != 0 || a.words != b.words) {
                        ++violations;
                    }
                    if (b.free_va_pages != a.free_va_pages - static_cast<int32_t>(pages)) {
                        ++violations;
                    }
                }
                pool = post;
                break;
            }
            case Outcome::Null:
                ++nulls;
                break;
            case Outcome::WouldCrash:
                ++crashes;
                break;
            case Outcome::Unmodelled:
                ++unmodelled;
                break;
            }
        }
    }
    std::printf("random: predicted %llu (fresh %llu, hits %llu), null %llu, would-crash %llu, unmodelled %llu, "
                "invariant violations %d\n",
                static_cast<unsigned long long>(predicted), static_cast<unsigned long long>(fresh),
                static_cast<unsigned long long>(hits), static_cast<unsigned long long>(nulls),
                static_cast<unsigned long long>(crashes), static_cast<unsigned long long>(unmodelled), violations);
    CHECK(predicted > 500);
    CHECK(hits > 100);
    CHECK(fresh > 100);
    CHECK(violations == 0);
}

void TestOutOfScopeRequests() {
    Pools pre = MakePools(MakeSet(0, {R(0x10000000, 16 * kP, 16 * kP)}, {}, 0), Empty(1), Empty(2));
    Pools post;
    Request r = Req(1, 4);
    r.mode = 1;
    CHECK(Predict(pre, r, nullptr, &post).outcome == Outcome::Unmodelled);
    r = Req(1, 4);
    r.va_hint = 0x10000000;
    CHECK(Predict(pre, r, nullptr, &post).outcome == Outcome::Unmodelled);
    r = Req(1, 4);
    r.node = 0x1000;
    CHECK(Predict(pre, r, nullptr, &post).outcome == Outcome::Unmodelled);
    r = Req(1, 4, 0x30000); // not a power of two
    CHECK(Predict(pre, r, nullptr, &post).outcome == Outcome::Unmodelled);
}

// The shadow map: splits at both ends, a remap over a hole, unknown ranges that an unmap or a map resolves.
void TestPhysMapSplit() {
    PhysMap m;
    uint64_t bit = 0;
    m.Map(0x10000000, 8 * kP, 100);
    m.Unmap(0x10000000 + 2 * kP, 3 * kP);
    CHECK(m.Lookup(0x10000000 + kP, &bit) == PhysMap::State::Mapped && bit == 101);
    CHECK(m.Lookup(0x10000000 + 2 * kP, &bit) == PhysMap::State::Unmapped);
    CHECK(m.Lookup(0x10000000 + 4 * kP, &bit) == PhysMap::State::Unmapped);
    CHECK(m.Lookup(0x10000000 + 5 * kP, &bit) == PhysMap::State::Mapped && bit == 105);
    CHECK(m.Lookup(0x10000000 + 7 * kP + 0x1234, &bit) == PhysMap::State::Mapped && bit == 107); // inside a page
    CHECK(m.Lookup(0x10000000 + 8 * kP, &bit) == PhysMap::State::Unmapped);
    CHECK(m.MappedRuns() == 2);

    m.Map(0x10000000 + kP, kP, 900); // a remap replaces the page and keeps its neighbours
    CHECK(m.Lookup(0x10000000 + kP, &bit) == PhysMap::State::Mapped && bit == 900);
    CHECK(m.Lookup(0x10000000, &bit) == PhysMap::State::Mapped && bit == 100);
    CHECK(m.MappedRuns() == 3);

    m.MarkUnknown(0x20000000, 4 * kP);
    CHECK(m.Lookup(0x20000000 + 3 * kP, &bit) == PhysMap::State::Unknown);
    CHECK(m.Lookup(0x20000000 + 4 * kP, &bit) == PhysMap::State::Unmapped);
    m.Unmap(0x20000000 + kP, kP); // known unmapped now; the rest stays unknown
    CHECK(m.Lookup(0x20000000 + kP, &bit) == PhysMap::State::Unmapped);
    CHECK(m.Lookup(0x20000000, &bit) == PhysMap::State::Unknown);
    CHECK(m.Lookup(0x20000000 + 2 * kP, &bit) == PhysMap::State::Unknown);
    m.Map(0x20000000, 4 * kP, 7); // a map resolves the whole range
    CHECK(m.UnknownRuns() == 0);
    CHECK(m.Lookup(0x20000000 + 3 * kP, &bit) == PhysMap::State::Mapped && bit == 10);
    m.Clear();
    CHECK(m.MappedRuns() == 0 && m.UnknownRuns() == 0);
}

// The batch ops (vmem_batch_map queue, spec 2.4): the Onion and Garlic bit of an offset, a failed flush,
// protection-only ops, and the ops the model does not track.
void TestBatchOps() {
    PhysMap m;
    uint64_t bit = 0;
    ApplyBatchOp(&m, 0x30000000, kPhysBaseOffset + 5 * kP, 2 * kP, 0, false);
    CHECK(m.Lookup(0x30000000 + kP, &bit) == PhysMap::State::Mapped && bit == 6);
    // Garlic: phys 0x40100000 is bit 0x4000 (the bit index is shared by both bitmaps).
    ApplyBatchOp(&m, 0x31000000, 0x40100000, kP, 0, false);
    CHECK(m.Lookup(0x31000000, &bit) == PhysMap::State::Mapped && bit == 0x4000);
    // A failed flush: the map op's range is unknown, the model cannot tell whether it took effect.
    ApplyBatchOp(&m, 0x32000000, kPhysBaseOffset, kP, 0, true);
    CHECK(m.Lookup(0x32000000, nullptr) == PhysMap::State::Unknown);
    // Offsets below the base or off the page grid are not understood.
    ApplyBatchOp(&m, 0x33000000, 0x1000, kP, 0, false);
    CHECK(m.Lookup(0x33000000, nullptr) == PhysMap::State::Unknown);
    ApplyBatchOp(&m, 0x34000000, kPhysBaseOffset + 0x800, kP, 0, false);
    CHECK(m.Lookup(0x34000000, nullptr) == PhysMap::State::Unknown);
    // Unmap: known unmapped; a failed unmap is unknown.
    ApplyBatchOp(&m, 0x30000000, 0, 2 * kP, 1, false);
    CHECK(m.Lookup(0x30000000, nullptr) == PhysMap::State::Unmapped);
    ApplyBatchOp(&m, 0x31000000, 0, kP, 1, true);
    CHECK(m.Lookup(0x31000000, nullptr) == PhysMap::State::Unknown);
    // Protect (op 4) and op 2 keep the mapping, failed or not.
    ApplyBatchOp(&m, 0x35000000, kPhysBaseOffset, kP, 0, false);
    ApplyBatchOp(&m, 0x35000000, 0, kP, 4, true);
    CHECK(m.Lookup(0x35000000, &bit) == PhysMap::State::Mapped && bit == 0);
    ApplyBatchOp(&m, 0x35000000, 0, kP, 2, false);
    CHECK(m.Lookup(0x35000000, &bit) == PhysMap::State::Mapped && bit == 0);
    // The phys base is the game's global: a base of 0x200000 moves the bit index by 0x10.
    ApplyBatchOp(&m, 0x37000000, 0x200000 + 3 * kP, kP, 0, false, 0x200000);
    CHECK(m.Lookup(0x37000000, &bit) == PhysMap::State::Mapped && bit == 3);
    // Op 3 (flexible) and unknown ops: not tracked.
    ApplyBatchOp(&m, 0x36000000, 0, kP, 3, false);
    CHECK(m.Lookup(0x36000000, nullptr) == PhysMap::State::Unknown);
}

// The seed: empty without a snapshot, empty when S1 and S2 are pristine, otherwise the S1 and S2 free runs
// are unknown (S0 is not reserve-mapped and stays as it is).
void TestSeedMap() {
    PhysMap m;
    CHECK(!SeedMap(&m, nullptr) && m.MappedRuns() == 0 && m.UnknownRuns() == 0);

    Pools pristine = MakePools(MakeSet(0, {R(0x10000000, 4 * kP, 4 * kP)}, {}, 0), Empty(1), Empty(2));
    CHECK(!SeedMap(&m, &pristine) && m.UnknownRuns() == 0);

    Set used2 = MakeSet(2, {R(0x30000000, 4 * kP, 4 * kP)}, std::vector<uint64_t>(1, 0), 0);
    used2.region_count = 1;
    Pools used = MakePools(MakeSet(0, {R(0x10000000, 4 * kP, 4 * kP)}, {}, 0),
                           MakeSet(1, {R(0x20000000, 2 * kP, 0)}, {0}, 0), used2);
    CHECK(SeedMap(&m, &used));
    uint64_t bit = 0;
    CHECK(m.Lookup(0x20000000, &bit) == PhysMap::State::Unknown);
    CHECK(m.Lookup(0x30000000 + 3 * kP, &bit) == PhysMap::State::Unknown);
    CHECK(m.Lookup(0x30000000 + 4 * kP, &bit) == PhysMap::State::Unmapped);
    CHECK(m.Lookup(0x10000000, &bit) == PhysMap::State::Unmapped);
}

// The node search of vmem_query_reserve_map's caller (0x1a205-0x1a2b0): keyed on the node's backed pages.
void TestSearchNodeKey() {
    const uint64_t a = 0x30000000, b = 0x40000000, c = 0x50000000;
    CHECK(SearchNode({R(a, 2 * kP, 0), R(b, 16 * kP, 2 * kP)}, 2) == 1); // size 2 pages, backed 0: not a match
    CHECK(SearchNode({R(a, 16 * kP, 2 * kP)}, 2) == 0);                  // exact match on backed pages
    CHECK(SearchNode({R(a, 1 * kP, 1 * kP)}, 2) == 0);                   // nothing above: the first node
    CHECK(SearchNode({R(a, 4 * kP, 4 * kP), R(b, 3 * kP, 3 * kP)}, 2) == 1); // the fewest above the request
    CHECK(SearchNode({R(a, 1 * kP, 1 * kP), R(b, 4 * kP, 4 * kP), R(c, 3 * kP, 3 * kP)}, 2) == 2);
    // The quirk (cmp rax, r8 / sete): the first node stays a candidate, so a later node with more backed
    // pages still replaces it.
    CHECK(SearchNode({R(a, 5 * kP, 5 * kP), R(b, 10 * kP, 10 * kP)}, 2) == 1);
}

// vmem_query_reserve_map flag 1 on a node that is larger than the request's 1 MiB-rounded remaining pages:
// the mapped pages come back to the bitmap at the Garlic bits, the front of the node is consumed, and the
// page loop takes the bits. The request does not fit the node (1 MiB alignment), so the VA is fresh.
void TestReserveMapPartialNode() {
    const uint64_t base = 0x30010000;
    const uint64_t words = 0x4000 / 64 + 1; // bits up to 0x4040: the Garlic bits are above 0x4000
    Pools pre = MakePools(Empty(0), Empty(1),
                          MakeSet(2, {R(base, 17 * kP, 17 * kP)}, std::vector<uint64_t>(words, 0), 0));
    PhysMap map;
    map.Map(base, 16 * kP, 0x4000);
    Pools post;
    Prediction p = Predict(pre, Req(2, 16, kP), &map, &post);
    CHECK(p.outcome == Outcome::Predicted);
    CHECK(p.fresh && p.slot == 2);
    CHECK(p.unmapped.size() == 1 && p.unmapped[0] == (Run{base, 16 * kP, 0}));
    CHECK(p.runs.size() == 1 && p.runs[0].set == 2 && p.runs[0].first_page == 0x4000 && p.runs[0].pages == 16 &&
          p.runs[0].protect_type == -1);
    CHECK(SameList(post.set[2].free_va, {R(base + 16 * kP, kP, kP)}));
    CHECK(post.set[2].free_va_pages == 1);
    CHECK(post.set[2].stored_free_count == 1);
    CHECK(post.set[2].cached_phys == 0);
    CHECK(post.set[2].words[0x4000 / 64] == 0);
    CHECK(post.set[2].backed_bytes == 16 * kP);
    CHECK(post.set[2].region_count == 1);

    // The same node with only 4 of its 16 consumed pages mapped: the scan takes 4 bits, the node's last page
    // is consumed by the next pass, the list is empty, and the no-bit scan is the POOL_FIX null read.
    PhysMap few;
    few.Map(base, 4 * kP, 0x4000);
    Prediction q = Predict(pre, Req(2, 16, kP), &few, &post);
    CHECK(q.outcome == Outcome::WouldCrash);

    // Unknown: a page of the consumed range the map does not know. Nothing is predicted for it.
    PhysMap unknown;
    unknown.MarkUnknown(base + 2 * kP, kP);
    Prediction u = Predict(pre, Req(2, 16, kP), &unknown, &post);
    CHECK(u.outcome == Outcome::Unmodelled && u.unknown_state);

    // No map, and a bit outside the bitmap: Unmodelled, not counted as unknown state.
    Prediction n = Predict(pre, Req(2, 16, kP), nullptr, &post);
    CHECK(n.outcome == Outcome::Unmodelled && !n.unknown_state);
    PhysMap wild;
    wild.Map(base, kP, 0x100000);
    Prediction w = Predict(pre, Req(2, 16, kP), &wild, &post);
    CHECK(w.outcome == Outcome::Unmodelled && !w.unknown_state);
}

} // namespace

// Each prediction path gets its class (the per-path counters of SOTC_PC_POOL_PATHS).
void TestPathClassification() {
    {
        // Flexible exact hit: the run is exactly the request, backed the same: no pad, no split, no node.
        Pools pre = MakePools(MakeSet(0, {R(0x10000000, 4 * kP, 4 * kP)}, {}, 0), Empty(1), Empty(2));
        Pools post;
        CHECK(Predict(pre, Req(1, 4), nullptr, &post).path == Path::FlexHitExact);
    }
    {
        // Flexible hit with a split (the TestFlexHit fixture): the tail takes a node.
        Pools pre = MakePools(MakeSet(0, {R(0x10000000, 16 * kP, 16 * kP)}, {}, 0), Empty(1), Empty(2));
        Pools post;
        CHECK(Predict(pre, Req(1, 4), nullptr, &post).path == Path::FlexHitNode);
    }
    {
        // Flexible hit with a pad: the run starts 1 MiB - 64 KiB below the aligned start the request needs.
        Pools pre = MakePools(MakeSet(0, {R(0x10000000 + 0xF0000, 8 * kP, 8 * kP)}, {}, 0), Empty(1), Empty(2));
        Pools post;
        Request r = Req(1, 2, kOneMiB);
        Prediction p = Predict(pre, r, nullptr, &post);
        CHECK(p.outcome == Outcome::Predicted);
        CHECK(p.path == Path::FlexHitNode);
    }
    {
        // Primary exact hit (kind 2 searches S2): no pad, no split.
        Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {R(0x30000000, 2 * kP, 2 * kP)}, {0}, 0));
        Pools post;
        CHECK(Predict(pre, Req(2, 2), nullptr, &post).path == Path::PrimaryHitExact);
    }
    {
        // Primary hit with a split (kind 1 searches S1 after S0 is empty).
        Pools pre = MakePools(Empty(0), MakeSet(1, {R(0x20000000, 16 * kP, 16 * kP)}, {0}, 0), Empty(2));
        Pools post;
        CHECK(Predict(pre, Req(1, 4), nullptr, &post).path == Path::PrimaryHitNode);
    }
    {
        // Fresh, bitmap only: S2 has cached pages and no free VA, so the page loop takes bits and never reserves.
        Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {}, {0x7}, 3));
        Pools post;
        Prediction p = Predict(pre, Req(2, 2), nullptr, &post);
        CHECK(p.outcome == Outcome::Predicted && p.fresh);
        CHECK(p.path == Path::FreshPlain);
    }
    {
        // Fresh with a spill to S1 (TestSpillToOnion).
        Pools pre = MakePools(Empty(0), MakeSet(1, {}, {0x7}, 3), Empty(2));
        Pools post;
        CHECK(Predict(pre, Req(2, 2), nullptr, &post).path == Path::FreshSpill);
    }
    {
        // Fresh with the reserve-map: S2 has a free node that does not fit aligned, cached pages zero, the map
        // knows its page (bit 5).
        Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {R(0x30010000, kP, kP)}, {0}, 0));
        Pools post;
        PhysMap map;
        map.Map(0x30010000, kP, 5);
        Prediction p = Predict(pre, Req(2, 1), &map, &post);
        CHECK(p.outcome == Outcome::Predicted);
        CHECK(p.path == Path::FreshReserve);
    }
    {
        // POOL_FIX: S2 reports 3 cached pages but has no free bit; the node is reserve-mapped.
        Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {R(0x30010000, 2 * kP, 2 * kP)}, {0}, 3));
        Pools post;
        PhysMap map;
        map.Map(0x30010000, kP, 5);
        Prediction p = Predict(pre, Req(2, 1), &map, &post);
        CHECK(p.outcome == Outcome::Predicted);
        CHECK(p.path == Path::FreshPoolFix);
    }
    {
        Pools pre = MakePools(Empty(0), Empty(1), Empty(2));
        Pools post;
        CHECK(Predict(pre, Req(2, 4), nullptr, &post).path == Path::EarlyReject);
    }
    {
        Pools pre = MakePools(Empty(0), Empty(1), MakeSet(2, {R(0x30000000, 3 * kP, 0)}, {0}, 3));
        Pools post;
        PhysMap map;
        CHECK(Predict(pre, Req(2, 4), &map, &post).path == Path::WouldCrash);
    }
    {
        Pools pre = MakePools(Empty(0), Empty(1), Empty(2));
        Pools post;
        Request r = Req(1, 1);
        r.va_hint = 0x1000;
        CHECK(Predict(pre, r, nullptr, &post).path == Path::Unmodelled);
    }
}

void TestPathMask() {
    CHECK(kDefaultPathMask == (PathBit(Path::FlexHitExact) | PathBit(Path::PrimaryHitExact)));
    CHECK(ParsePathMask("", nullptr) == 0);
    CHECK(ParsePathMask("none", nullptr) == 0);
    CHECK(ParsePathMask("all", nullptr) == (uint32_t{1} << kPathCount) - 1);
    CHECK(ParsePathMask("flex-exact, primary-node", nullptr) ==
          (PathBit(Path::FlexHitExact) | PathBit(Path::PrimaryHitNode)));
    for (int i = 0; i < kPathCount; ++i) {
        const Path path = static_cast<Path>(i);
        CHECK(ParsePathMask(PathName(path), nullptr) == PathBit(path));
    }
    // Unknown names are reported and ignored; the valid ones still apply.
    std::string bad;
    CHECK(ParsePathMask("bogus,primary-exact", &bad) == PathBit(Path::PrimaryHitExact));
    CHECK(bad == "bogus");
}

// The native rule: a predicted call on a masked path. Unmodelled and WouldCrash never run natively, even
// with every path in the mask. An empty mask is the kill switch: nothing runs natively.
void TestEligible() {
    Pools pre = MakePools(MakeSet(0, {R(0x10000000, 4 * kP, 4 * kP)}, {}, 0), Empty(1), Empty(2));
    Pools post;
    Prediction hit = Predict(pre, Req(1, 4), nullptr, &post);
    CHECK(Eligible(hit, kDefaultPathMask));
    CHECK(!Eligible(hit, 0));
    Prediction reserve_like = hit;
    reserve_like.path = Path::FreshReserve;
    CHECK(!Eligible(reserve_like, kDefaultPathMask));
    CHECK(Eligible(reserve_like, PathBit(Path::FreshReserve)));

    const uint32_t all = (uint32_t{1} << kPathCount) - 1;
    Pools crash_pre = MakePools(Empty(0), Empty(1), MakeSet(2, {R(0x30000000, 3 * kP, 0)}, {0}, 3));
    PhysMap map;
    Prediction crash = Predict(crash_pre, Req(2, 4), &map, &post);
    CHECK(crash.outcome == Outcome::WouldCrash);
    CHECK(!Eligible(crash, all));

    Pools unmod_pre = MakePools(Empty(0), Empty(1), Empty(2));
    Request r = Req(1, 1);
    r.va_hint = 0x1000;
    Prediction unmod = Predict(unmod_pre, r, nullptr, &post);
    CHECK(unmod.outcome == Outcome::Unmodelled);
    CHECK(!Eligible(unmod, all));

    // An early reject is a Null outcome: it is eligible only when its path is in the mask.
    Pools empty_pre = MakePools(Empty(0), Empty(1), Empty(2));
    Prediction null_pred = Predict(empty_pre, Req(2, 4), nullptr, &post);
    CHECK(null_pred.outcome == Outcome::Null);
    CHECK(Eligible(null_pred, all));
    CHECK(!Eligible(null_pred, kDefaultPathMask));
}

// ---- native decision (Native::Controller) over a fake host -------------------------------------------
// The fake host keeps a world of the three sets. Predict predicts "alloc_bytes += size" on S0 with a fixed VA.
// ApplyExactHit makes that change plus `extra` (extra != 0 is a native result the prediction does not match),
// reports `writes` as its write set, and returns apply_node (0: refused). The GuestMem side (SotcPcPoolNative)
// is not in this unit: it is covered by the native tests; here the decision is what is under test.
constexpr uint64_t kFakeVa = 0x30000000;

class FakeHost final : public Native::Host {
public:
    Pools world;
    Path path = Path::FlexHitExact;   ///< the path Predict reports
    bool take_ok = true;
    bool snapshot_ok = true;
    bool observe_ok = true;
    bool in_modelled = true;          ///< InModelledMemory answer for every range
    uint64_t apply_node = 0x20000;    ///< ApplyExactHit's return (0: refused)
    uint64_t extra = 0;               ///< bytes ApplyExactHit adds beyond the prediction
    bool partial_on_refusal = false;  ///< a refusal that already wrote (the native side's documented exception)
    int lock_held = 0;
    int takes = 0;
    int releases = 0;
    int predicts = 0;
    int applies = 0;
    int tails = 0;
    uint64_t last_size = 0;
    std::vector<std::pair<uint64_t, size_t>> writes;
    std::vector<std::string> logs;

    bool TakeLock() override {
        if (!take_ok) {
            return false;
        }
        ++takes;
        lock_held = 1;
        return true;
    }
    void ReleaseLock() override {
        ++releases;
        lock_held = 0;
    }
    bool Snapshot(Pools* out) override {
        if (!lock_held) {
            ++logs_lock_violations;
        }
        if (!snapshot_ok) {
            return false;
        }
        *out = world;
        return true;
    }
    void Predict(const Pools& pre, const Request& req, Prediction* p, Pools* predicted) override {
        ++predicts;
        *predicted = pre;
        predicted->set[0].alloc_bytes += req.size;
        *p = Prediction{};
        p->outcome = Outcome::Predicted;
        p->path = path;
        p->slot = 0;
        p->size = req.size;
        p->va = kFakeVa;
    }
    uint64_t ApplyExactHit(const Pools& pre, const Prediction& p, const Request& req) override {
        (void)pre;
        (void)p;
        ++applies;
        last_size = req.size;
        writes.clear();
        if (apply_node == 0) {
            if (partial_on_refusal) {
                writes.push_back({0x1000, 8});
            }
            return 0;
        }
        world.set[0].alloc_bytes += req.size + extra;
        writes.push_back({0x1000, 8});
        return apply_node;
    }
    std::vector<std::pair<uint64_t, size_t>> LastWriteSet() override { return writes; }
    bool InModelledMemory(const Pools& pre, uint64_t addr, size_t n) override {
        (void)pre;
        (void)addr;
        (void)n;
        return in_modelled;
    }
    bool Observe(uint64_t node, Observed* got) override {
        (void)node;
        if (!observe_ok) {
            return false;
        }
        got->va = kFakeVa;
        got->size = last_size;
        got->slot = 0;
        got->post = world;
        return true;
    }
    void Tail() override { ++tails; }
    void Log(const std::string& text) override { logs.push_back(text); }

    int logs_lock_violations = 0;
};

Pools FakePools() { return MakePools(Empty(0), Empty(1), Empty(2)); }

void TestNativeEligibleApplied() {
    FakeHost h;
    h.world = FakePools();
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(node == 0x20000);
    CHECK(h.applies == 1 && h.tails == 1);
    CHECK(h.takes == 1 && h.releases == 1 && h.lock_held == 0);
    CHECK(!c.killed() && c.stats().verified == 1 && c.stats().native_calls == 1);
    CHECK(h.logs.empty());
    // The next call is predicted from the state the native call left: it is native too.
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(h.applies == 2 && c.stats().verified == 2);
    CHECK(h.logs_lock_violations == 0);
}

void TestNativeIneligibleRunsOriginal() {
    FakeHost h;
    h.world = FakePools();
    h.path = Path::FreshPlain; // not in the default mask
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 7;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    CHECK(node == 0);
    CHECK(h.predicts == 1 && h.applies == 0 && h.tails == 0);
    CHECK(h.releases == 1 && h.lock_held == 0);
    CHECK(c.stats().fallback[static_cast<int>(Native::Fallback::Ineligible)] == 1);
    CHECK(!c.killed());
    // A path in the mask is eligible: the same controller with FreshPlain allowed runs it natively.
    Native::Controller all(PathBit(Path::FreshPlain));
    CHECK(all.Run(h, Req(1, 4), &node) == Native::Route::Native);
}

void TestNativeRefusalRunsOriginal() {
    FakeHost h;
    h.world = FakePools();
    h.apply_node = 0;
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    CHECK(h.applies == 1 && h.tails == 0 && h.releases == 1);
    CHECK(c.stats().fallback[static_cast<int>(Native::Fallback::Refused)] == 1);
    CHECK(h.logs.empty() && !c.killed());
    // A refusal is not a kill: the next call may still run natively.
    h.apply_node = 0x20000;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
}

void TestNativeRefusalAfterWritesKills() {
    FakeHost h;
    h.world = FakePools();
    h.apply_node = 0;
    h.partial_on_refusal = true;
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    CHECK(c.killed() && c.stats().kills == 1);
    CHECK(h.logs.size() == 1);
}

void TestNativeLockBusyRunsOriginal() {
    FakeHost h;
    h.world = FakePools();
    h.take_ok = false;
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    CHECK(h.predicts == 0 && h.applies == 0 && h.releases == 0);
    CHECK(c.stats().fallback[static_cast<int>(Native::Fallback::LockBusy)] == 1);
    CHECK(!c.killed());
}

void TestNativeSnapshotFailureRunsOriginal() {
    FakeHost h;
    h.world = FakePools();
    h.snapshot_ok = false;
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    CHECK(h.predicts == 0 && h.applies == 0 && h.releases == 1);
    CHECK(c.stats().fallback[static_cast<int>(Native::Fallback::SnapshotFailed)] == 1);
}

void TestNativeMismatchKillsOnce() {
    FakeHost h;
    h.world = FakePools();
    h.extra = 1; // the native result differs from the prediction
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    // The state is changed, so the call returns natively with the node; it is never run twice.
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(node == 0x20000);
    CHECK(c.killed() && c.stats().kills == 1 && c.stats().verified == 0);
    CHECK(h.tails == 1 && h.releases == 1 && h.lock_held == 0);
    CHECK(h.logs.size() == 1);
    CHECK(h.logs[0].find("KILLED") != std::string::npos);
    // Kill is final: no further native application, no retry, one log line.
    h.extra = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    CHECK(h.applies == 1 && h.predicts == 1);
    CHECK(c.stats().fallback[static_cast<int>(Native::Fallback::Killed)] == 2);
    CHECK(h.logs.size() == 1 && c.stats().kills == 1);
}

void TestNativeWriteOutsideModelledKills() {
    FakeHost h;
    h.world = FakePools();
    h.in_modelled = false;
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(c.killed());
    CHECK(h.logs.size() == 1 && h.logs[0].find("outside the modelled memory") != std::string::npos);
}

void TestNativePostReadFailureKills() {
    FakeHost h;
    h.world = FakePools();
    h.observe_ok = false;
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(c.killed());
}

void TestNativePeriodicDriftKills() {
    FakeHost h;
    h.world = FakePools();
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    // 63 native calls: the expectation is valid, the 64th call is a checkpoint.
    for (int i = 0; i < 63; ++i) {
        CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    }
    CHECK(c.stats().checks_full == 0);
    // An unseen change between two native calls (not a free, not an original call): the checkpoint counts it and
    // logs it (Controller::Run: not a kill, since every native call re-reads and verifies its own state). The call
    // then runs natively as usual.
    h.world.set[0].alloc_bytes += 4096;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(!c.killed() && c.stats().checks_full == 1 && c.stats().kills == 0);
    CHECK(h.logs.size() == 1);
}

void TestNativePeriodicCleanPasses() {
    FakeHost h;
    h.world = FakePools();
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    for (int i = 0; i < 130; ++i) {
        CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    }
    CHECK(!c.killed() && c.stats().checks_full == 2 && c.stats().verified == 130);
    CHECK(c.stats().checks_skipped == 0);
}

void TestNativePeriodicSkippedWithoutExpectation() {
    FakeHost h;
    h.world = FakePools();
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    // Calls 1-62 native, call 63 runs the original (the expectation is void), call 64 is a checkpoint.
    for (int i = 0; i < 62; ++i) {
        CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    }
    h.path = Path::FreshPlain;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Original);
    h.path = Path::FlexHitExact;
    // The original changed the state in the game; the model cannot know how, so the checkpoint is skipped.
    h.world.set[0].alloc_bytes += 8192;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(!c.killed() && c.stats().checks_skipped == 1 && c.stats().checks_full == 0);
}

void TestNativeFreeVoidsExpectation() {
    FakeHost h;
    h.world = FakePools();
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    for (int i = 0; i < 63; ++i) {
        CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    }
    c.NoteUnseenChange(); // a free the hook saw
    h.world.set[0].alloc_bytes += 4096;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    CHECK(!c.killed() && c.stats().checks_skipped == 1 && c.stats().checks_full == 0);
}

// A fresh block pops its node in the call (0x19fbf). The pop refills the node pool only from count 3 (3 -> 2, 0x19660).
void TestFreshPopRefills() {
    CHECK(IsFreshPath(Path::FreshPlain));
    CHECK(IsFreshPath(Path::FreshReserve));
    CHECK(IsFreshPath(Path::FreshSpill));
    CHECK(IsFreshPath(Path::FreshPoolFix));
    CHECK(!IsFreshPath(Path::FlexHitExact));
    CHECK(!IsFreshPath(Path::PrimaryHitNode));
    CHECK(!IsFreshPath(Path::EarlyReject));
    CHECK(FreshPopRefills(Path::FreshPlain, 3));
    CHECK(FreshPopRefills(Path::FreshReserve, 3));
    CHECK(FreshPopRefills(Path::FreshSpill, 3));
    CHECK(FreshPopRefills(Path::FreshPoolFix, 3));
    // Other counts pop without a refill (the set state is unchanged by the pop).
    CHECK(!FreshPopRefills(Path::FreshPlain, 2));
    CHECK(!FreshPopRefills(Path::FreshPlain, 4));
    CHECK(!FreshPopRefills(Path::FreshPlain, 0));
    // The hit paths are PredictNodeCall's (never this predicate).
    CHECK(!FreshPopRefills(Path::FlexHitNode, 3));
    CHECK(!FreshPopRefills(Path::PrimaryHitNode, 3));
    CHECK(!FreshPopRefills(Path::EarlyReject, 3));
}

// Every announced change is attributed to its source; an announced change never counts as periodic_outside.
void TestNativeAttributedInvalidation() {
    FakeHost h;
    h.world = FakePools();
    Native::Controller c(kDefaultPathMask);
    uint64_t node = 0;
    for (int i = 0; i < 5; ++i) {
        CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    }
    c.NoteUnseenChange("direct 0x1a660");
    c.NoteUnseenChange("direct 0x1a660");
    c.NoteUnseenChange("free");
    c.NoteUnseenChange();
    CHECK(c.stats().invalidated.at("direct 0x1a660") == 2);
    CHECK(c.stats().invalidated.at("free") == 1);
    CHECK(c.stats().invalidated.at("unattributed") == 1);
    CHECK(c.stats().periodic_outside == 0);
    // The announced change is in the state; the next call re-reads it and verifies, and the checkpoint (call 64)
    // finds the expectation consistent: nothing counted outside.
    h.world.set[0].alloc_bytes += 4096;
    CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    for (int i = 0; i < 70; ++i) {
        CHECK(c.Run(h, Req(1, 4), &node) == Native::Route::Native);
    }
    CHECK(!c.killed() && c.stats().periodic_outside == 0 && c.stats().kills == 0);
}

void TestNativeKindsAndNames() {
    CHECK(std::string(Native::FallbackName(Native::Fallback::LockBusy)) == "lock busy");
    CHECK(std::string(Native::FallbackName(Native::Fallback::Killed)) == "killed");
    CHECK(Native::Controller::kCheckInterval == 64);
}

int main() {
    TestNativeEligibleApplied();
    TestNativeIneligibleRunsOriginal();
    TestNativeRefusalRunsOriginal();
    TestNativeRefusalAfterWritesKills();
    TestNativeLockBusyRunsOriginal();
    TestNativeSnapshotFailureRunsOriginal();
    TestNativeMismatchKillsOnce();
    TestNativeWriteOutsideModelledKills();
    TestNativePostReadFailureKills();
    TestNativePeriodicDriftKills();
    TestNativePeriodicCleanPasses();
    TestNativePeriodicSkippedWithoutExpectation();
    TestNativeFreeVoidsExpectation();
    TestNativeKindsAndNames();
    TestFreshPopRefills();
    TestNativeAttributedInvalidation();
    TestPathClassification();
    TestPathMask();
    TestEligible();
    TestPhysMapSplit();
    TestBatchOps();
    TestSeedMap();
    TestSearchNodeKey();
    TestReserveMapPartialNode();
    TestFlexHit();
    TestFlexTailDropWhenNotBacked();
    TestOnionHitKeepsBitmap();
    TestOneMiBAlignmentKeepsPad();
    TestGarlicFreshUsesOwnBitmap();
    TestSpillToOnion();
    TestSpillToGarlicProtectType();
    TestFallbackVaNotUsed();
    TestEarlyReject();
    TestReserveMapWithMap();
    TestReserveMapNeedsMap();
    TestWouldCrashWhenNoNodeLeft();
    TestCompare();
    TestCompareFresh();
    TestFreeAccounting();
    TestOutOfScopeRequests();
    TestRandomInvariants();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
