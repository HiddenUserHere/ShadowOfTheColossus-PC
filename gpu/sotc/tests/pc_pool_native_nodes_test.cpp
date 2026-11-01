// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-native-nodes-test. Checks gpu/sotc/pc_pool_native_nodes.cpp (PopNode, PredictPop, PredictNodeCall)
// against a literal transcription of pool_node_alloc (0x19660) with its refill: the pop stores P1..P7, the refill's
// bookkeeping add, the refill's exact hit (ApplyExactHit's store list for the single-run configuration used here, W1..W22),
// and the carve of 585 nodes (C1..C17 per node). The expected image is written by hand below, with the stores in the
// game's order; PopNode performs the refill's exact hit before the pop stores, so the final images are compared
// byte for byte (that comparison also checks that the two orders agree).
//
// Build and run (scratch directory, not the repo):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_native_nodes_test.cpp -o pc_pool_native_nodes_test.exe
//   ./pc_pool_native_nodes_test.exe   (prints PASS or FAIL; exit code 0 on PASS)
//
// A hand transcription, not an emulator: a shared misreading of the disassembly would pass both. The in-game
// comparison (the PC_POOL hooks) is the check of the real machine.

#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"
#include "../pc_pool_native.cpp"
#include "../pc_pool_native_nodes.cpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

using namespace SotcPcPool::Model;
using SotcPcPoolNative::GuestMem;
using SotcPcPoolNodes::PopNode;
using SotcPcPoolNodes::PopPrediction;
using SotcPcPoolNodes::Status;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                   \
            ++g_failures;                                                \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);       \
        }                                                                \
    } while (0)

constexpr uint64_t kRoot = 0x10000000;
constexpr uint64_t kS0 = kRoot;
constexpr uint64_t kSent = kRoot + 0x1e0;   // pool list sentinel
constexpr uint64_t kTailLink = kRoot + 0x1e8;
constexpr uint64_t kHeadLink = kRoot + 0x1f0;
constexpr uint64_t kCnt = kRoot + 0x200;
constexpr uint64_t kTagFlex = kRoot + 0x20d918; // owner tags, flexible set
constexpr uint64_t kFB = 0x200000000ull;         // flexible VA base (root+0x20d908)
constexpr uint64_t kNodeArea = 0x60000000ull;
constexpr uint64_t kOver = kNodeArea + 0x8000;   // DAT_02dfca60 stand-in
constexpr uint64_t kPool0 = kNodeArea + 0x4000;  // pool-list nodes: kPool0 + i * 0x100
constexpr uint64_t kNodeB = kNodeArea;           // the free-VA node of the single run (scenario A)
constexpr uint64_t k64K = 0x10000;
constexpr uint64_t kSetOff[3] = {0x0, 0xa0, 0x140};

uint64_t PoolNode(int i) { return kPool0 + static_cast<uint64_t>(i) * 0x100; }

// ---------------------------------------------------------------------------------------------------
// Simulated guest memory: mapped ranges, sparse bytes (unwritten bytes read as 0).
struct FakeMem : GuestMem {
    std::vector<std::pair<uint64_t, uint64_t>> regions;
    std::map<uint64_t, uint8_t> bytes;

    bool Mapped(uint64_t a, size_t n) const {
        if (n == 0) {
            return false;
        }
        for (const auto& r : regions) {
            if (a >= r.first && a + n <= r.second && a + n >= a) {
                return true;
            }
        }
        return false;
    }
    bool Read(uint64_t a, void* out, size_t n) override {
        if (!Mapped(a, n)) {
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            auto it = bytes.find(a + i);
            static_cast<uint8_t*>(out)[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(uint64_t a, const void* in, size_t n) override {
        if (!Mapped(a, n)) {
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            bytes[a + i] = static_cast<const uint8_t*>(in)[i];
        }
        return true;
    }
    uint8_t Get(uint64_t a) const {
        auto it = bytes.find(a);
        return it == bytes.end() ? 0 : it->second;
    }
    uint64_t U64(uint64_t a) const {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | Get(a + static_cast<uint64_t>(i));
        }
        return v;
    }
    // Little-endian store of n bytes of v (the expected image is written with this, one call per game store).
    void Put(uint64_t a, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            bytes[a + static_cast<uint64_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    void MapAll() {
        regions.clear();
        regions.push_back({kRoot, kRoot + 0x230000});
        regions.push_back({kFB, kFB + 0x100000});
        regions.push_back({kNodeArea, kNodeArea + 0x10000000});
    }
};

bool SameImage(const FakeMem& a, const FakeMem& b) {
    for (const auto& kv : a.bytes) {
        if (b.Get(kv.first) != kv.second) {
            return false;
        }
    }
    for (const auto& kv : b.bytes) {
        if (a.Get(kv.first) != kv.second) {
            return false;
        }
    }
    return true;
}

// Write-fault and bit-flip wrapper: counts the Writes of the call (ApplyExactHit's and the pop's, in order).
struct MutMem : GuestMem {
    FakeMem* inner = nullptr;
    int index = 0;
    int fail_at = -1;
    int flip_at = -1;
    bool Read(uint64_t a, void* out, size_t n) override { return inner->Read(a, out, n); }
    bool Write(uint64_t a, const void* in, size_t n) override {
        const int i = index++;
        if (i == fail_at) {
            return false;
        }
        if (i == flip_at) {
            std::vector<uint8_t> tmp(static_cast<const uint8_t*>(in), static_cast<const uint8_t*>(in) + n);
            tmp[0] ^= 0x01;
            return inner->Write(a, tmp.data(), n);
        }
        return inner->Write(a, in, n);
    }
};

// ---------------------------------------------------------------------------------------------------
// Snapshot to guest memory.
Set MakeSet(int id, std::vector<Run> runs, int32_t region_count, int32_t cached, uint64_t alloc, uint64_t backed) {
    Set s;
    s.id = id;
    s.has_bitmap = id != 0;
    s.region_count = region_count;
    s.cached_phys = cached;
    s.alloc_bytes = alloc;
    s.backed_bytes = backed;
    uint64_t pages = 0;
    for (const Run& r : runs) {
        pages += r.size / k64K;
    }
    s.free_va_pages = static_cast<int32_t>(pages);
    s.stored_free_count = static_cast<int32_t>(runs.size());
    s.free_va = std::move(runs);
    return s;
}

Pools MakePools(Set s0, Set s1, Set s2) {
    Pools p;
    p.set[0] = std::move(s0);
    p.set[1] = std::move(s1);
    p.set[2] = std::move(s2);
    return p;
}

Set Empty(int id) { return MakeSet(id, {}, 0, 0, 0, 0); }

// The image of the snapshot: set headers, the free-VA list (no region elements in these scenarios), the pool list
// with pool_nodes nodes and count_field at root+0x200. break_link corrupts the second node's prev link.
void BuildImage(FakeMem* m, const Pools& p, int pool_nodes, int32_t count_field, bool break_link) {
    m->bytes.clear();
    m->Put(kRoot + 0x20d908, kFB, 8);
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = kRoot + kSetOff[k];
        const Set& s = p.set[k];
        if (s.region_count != 0) {
            std::printf("  test bug: region elements are not part of these scenarios\n");
            g_failures++;
        }
        m->Put(S + 0x10, S, 8);
        m->Put(S + 0x8, S, 8);
        m->Put(S + 0x20, 0, 4);
        const uint64_t F = S + 0x28;
        std::vector<uint64_t> el;
        for (size_t i = 0; i < s.free_va.size(); ++i) {
            const Run& r = s.free_va[i];
            const uint64_t N = kNodeArea + static_cast<uint64_t>(k) * 0x1000 + i * 0x100;
            el.push_back(N + 0x20);
            m->Put(N + 0x50, r.backed, 8);
            m->Put(N + 0x58, r.size, 8);
            m->Put(N + 0x60, r.base, 8);
            m->Put(N + 0x68, static_cast<uint64_t>(k), 1);
            m->Put(N + 0x69, 0, 1);
            m->Put(N + 0x48, 0xffffffffu, 4);
        }
        for (size_t i = 0; i < el.size(); ++i) {
            const uint64_t E = el[i];
            m->Put(E, E - 0x20, 8);
            m->Put(E + 8, i == 0 ? F : el[i - 1], 8);
            m->Put(E + 0x10, i + 1 == el.size() ? F : el[i + 1], 8);
            m->Put(E + 0x18, S, 8);
        }
        m->Put(S + 0x38, el.empty() ? F : el[0], 8);
        m->Put(S + 0x30, el.empty() ? F : el.back(), 8);
        m->Put(S + 0x48, static_cast<uint64_t>(s.stored_free_count), 4);
        m->Put(S + 0x70, static_cast<uint64_t>(s.free_va_pages), 4);
        m->Put(S + 0x74, static_cast<uint64_t>(s.cached_phys), 4);
        m->Put(S + 0x78, static_cast<uint64_t>(k), 4);
        m->Put(S + 0x80, s.alloc_bytes, 8);
        m->Put(S + 0x88, s.alloc_bytes, 8); // peak = current: the exact hit's new value is above it
        m->Put(S + 0x90, s.backed_bytes, 8);
        m->Put(S + 0x98, s.backed_bytes, 8);
    }
    // The pool list: sentinel S = root+0x1e0; head link root+0x1f0, tail link root+0x1e8.
    std::vector<uint64_t> el;
    for (int i = 0; i < pool_nodes; ++i) {
        el.push_back(PoolNode(i) + 0x20);
    }
    for (size_t i = 0; i < el.size(); ++i) {
        const uint64_t E = el[i];
        m->Put(E, E - 0x20, 8);
        m->Put(E + 8, i == 0 ? kSent : el[i - 1], 8);
        m->Put(E + 0x10, i + 1 == el.size() ? kSent : el[i + 1], 8);
        m->Put(E - 0x20 + 0x38, 0x1234, 8);      // owner list (pop clears it)
        m->Put(E - 0x20 + 0x40, 0x4444, 8);      // not touched by the pop
        m->Put(E - 0x20 + 0x48, 0x77777777u, 4); // not touched by the pop
    }
    if (break_link && el.size() >= 2) {
        m->Put(el[1] + 8, el[0] + 0x100, 8);     // E2.prev no longer names E1
    }
    m->Put(kHeadLink, el.empty() ? kSent : el[0], 8);
    m->Put(kTailLink, el.empty() ? kSent : el.back(), 8);
    m->Put(kCnt, static_cast<uint64_t>(static_cast<uint32_t>(count_field)), 4);
    m->Put(kOver, 0x500000, 8);
}

// ---------------------------------------------------------------------------------------------------
// The hand transcription of pool_node_alloc with a single exact-hit refill. Scenario A's configuration:
//   S0: one free run NB = {base kFB, size 64 KiB, backed 64 KiB}, alloc = backed = 0x30000 (peaks equal), pool 0.
//   Pool list: P1, P2, P3 (count 3). The pop takes P1; the refill takes NB.
// The stores are the game's, in the game's order (pop, counter, refill's exact hit, carve).
void ExpectRefillA(FakeMem& e) {
    const uint64_t N1 = PoolNode(0), E1 = N1 + 0x20, E2 = PoolNode(1) + 0x20, E3 = PoolNode(2) + 0x20;
    // Pop (0x19660, 0x1968f-0x196c3). prev of E1 is the sentinel, next is E2.
    e.Put(N1 + 0x38, 0, 8);          // P1 [N+0x38] := 0
    e.Put(N1 + 0x20, 0, 8);          // P2 [N+0x20] := 0
    e.Put(kSent + 0x10, E2, 8);      // P3 [prev+0x10] := next, prev = sentinel: root+0x1f0 := E2
    e.Put(E2 + 0x8, kSent, 8);       // P4 [next+0x8] := prev
    e.Put(N1 + 0x30, E1, 8);         // P5 [N+0x30] := E
    e.Put(N1 + 0x28, E1, 8);         // P6 [N+0x28] := E
    e.Put(kCnt, 2, 4);               // P7 count := 2
    // Refill: DAT_02dfca60 += 0x10000 (0x196d2).
    e.Put(kOver, 0x510000, 8);
    // Refill exact hit (ApplyExactHit, W1..W22) on S0: run NB, element EB = NB+0x20, F = S0+0x28, obj NULL.
    const uint64_t NB_ = kNodeB, EB = NB_ + 0x20, F = kS0 + 0x28;
    e.Put(F + 0x10, F, 8);           // W1 [prev+0x10] := next (prev = next = F): S0+0x38 := F
    e.Put(F + 0x8, F, 8);            // W2 [next+8] := prev: S0+0x30 := F
    e.Put(NB_ + 0x30, EB, 8);        // W3 dead store (overwritten by W7)
    e.Put(kS0 + 0x48, 0, 4);         // W4 free count 1 -> 0
    e.Put(EB, NB_, 8);               // W5 E+0 := N
    e.Put(EB + 0x8, kS0, 8);         // W6 E+8 := S
    e.Put(EB + 0x10, kS0, 8);        // W7 E+0x10 := old region head (S, list empty)
    e.Put(kS0 + 0x10, EB, 8);        // W8 region head := E
    e.Put(kS0 + 0x8, EB, 8);         // W9 [old head+8] := E (old head = S: region tail)
    e.Put(kS0 + 0x20, 1, 4);         // W10 region count 0 -> 1
    e.Put(NB_ + 0x38, kS0, 8);       // W11 owner list := S
    e.Put(kS0 + 0x90, 0x40000, 8);   // W12 backed := 0x30000 + 0x10000
    e.Put(kS0 + 0x98, 0x40000, 8);   // W13 backed peak (new value above the old peak)
    e.Put(kS0 + 0x80, 0x40000, 8);   // W14 alloc
    e.Put(kS0 + 0x88, 0x40000, 8);   // W15 alloc peak
    e.Put(NB_ + 0x68, 0, 1);         // W16 pool id
    e.Put(NB_ + 0x58, k64K, 8);      // W17 size
    e.Put(NB_ + 0x69, 1, 1);         // W18 in use
    e.Put(NB_ + 0x40, 0, 8);         // W19 owner family (obj NULL)
    e.Put(NB_ + 0x48, 0xffffffffu, 4); // W20
    e.Put(kTagFlex, 0, 1);           // W21 owner tag: one 64 KiB unit, tag 0
    e.Put(kS0 + 0x70, 0, 4);         // W22 free pages 1 -> 0
    // Carve (0x19722-0x197b3) of the block at kFB: 585 nodes, each C1..C17. tail starts at E3 (the pop left it).
    uint64_t tail = E3;
    int32_t cnt = 2;
    for (uint64_t k = 0; k < 585; ++k) {
        const uint64_t N = kFB + k * 0x70, E = N + 0x20;
        e.Put(N + 0x00, 0, 8);            // C1
        e.Put(N + 0x08, N, 8);            // C2
        e.Put(N + 0x10, N, 8);            // C3
        e.Put(N + 0x18, 0, 8);            // C4
        e.Put(N + 0x28, E, 8);            // C5 (dead)
        e.Put(N + 0x69, 0, 1);            // C6
        e.Put(N + 0x50, 0, 8);            // C7 (16 zero bytes: 0x50..0x5f)
        e.Put(N + 0x58, 0, 8);
        e.Put(N + 0x60, 0, 8);            // C8
        e.Put(N + 0x40, 0, 8);            // C9
        e.Put(N + 0x48, 0xffffffffu, 4);  // C10
        e.Put(N + 0x20, N, 8);            // C11
        e.Put(N + 0x30, kSent, 8);        // C12 next := sentinel
        e.Put(N + 0x28, tail, 8);         // C13 prev := old tail
        e.Put(kTailLink, E, 8);           // C14 tail := E
        e.Put(tail + 0x10, E, 8);         // C15 old tail's next := E
        ++cnt;
        e.Put(kCnt, static_cast<uint64_t>(static_cast<uint32_t>(cnt)), 4); // C16
        e.Put(N + 0x38, kSent, 8);        // C17 owner list := sentinel
        tail = E;
    }
}

// Scenario A's pre-state.
Pools PreA() {
    return MakePools(MakeSet(0, {Run{kFB, k64K, k64K, kNodeB}}, 0, 0, 0x30000, 0x30000), Empty(1), Empty(2));
}

// Counts of the expected write log of scenario A: the nested exact hit (22), the pop (7), the counter (1), and the
// carve (17 per node, 585 nodes).
constexpr size_t kWritesA = 22 + 7 + 1 + 585 * 17;

// ---------------------------------------------------------------------------------------------------
void TestPopWithRefill() {
    std::printf("pop with refill (exact-hit nested, count 3 -> 2 -> 587):\n");
    FakeMem pre, mem, expect;
    pre.MapAll();
    BuildImage(&pre, PreA(), 3, 3, false);
    mem = pre;
    expect = pre;
    ExpectRefillA(expect);

    uint64_t node = 0;
    std::vector<SotcPcPoolNodes::Write> writes;
    Status st = Status::Unknown;
    const bool ok = PopNode(mem, kRoot, PreA(), &node, &writes, kOver, &st);
    CHECK(ok);
    CHECK(st == Status::Popped);
    CHECK(node == PoolNode(0));
    CHECK(writes.size() == kWritesA);
    CHECK(SameImage(mem, expect));
    CHECK(mem.U64(kCnt) == 587);
    CHECK(mem.U64(kHeadLink) == PoolNode(1) + 0x20);
    CHECK(mem.U64(kTailLink) == kFB + 584 * 0x70 + 0x20);
    CHECK(mem.U64(kOver) == 0x510000);
    CHECK(mem.U64(kS0 + 0x70) == 0);
    CHECK(mem.U64(kS0 + 0x80) == 0x40000);
    // The node the model predicts for the same state, against the image.
    const PopPrediction pp = SotcPcPoolNodes::PredictPop(PreA(), 3);
    CHECK(pp.ok && pp.refill);
    CHECK(pp.count_post == 587);
    CHECK(pp.block_va == kFB && pp.block_nodes == 585);
    CHECK(pp.overhead_delta == k64K);
    CHECK(pp.post.set[0].free_va.empty());
    CHECK(pp.post.set[0].free_va_pages == static_cast<int32_t>(mem.U64(kS0 + 0x70)));
    CHECK(pp.post.set[0].alloc_bytes == mem.U64(kS0 + 0x80));
    CHECK(pp.post.set[0].backed_bytes == mem.U64(kS0 + 0x90));
    CHECK(pp.post.set[0].region_count == static_cast<int32_t>(mem.U64(kS0 + 0x20)));
    CHECK(pp.post.set[0].stored_free_count == static_cast<int32_t>(mem.U64(kS0 + 0x48) & 0xffffffffu));
}

void TestPopWithoutRefill() {
    std::printf("pop without refill (count 4 -> 3):\n");
    FakeMem pre, mem, expect;
    pre.MapAll();
    BuildImage(&pre, PreA(), 4, 4, false);
    mem = pre;
    expect = pre;
    // Hand transcription: P1..P7 only.
    const uint64_t N1 = PoolNode(0), E1 = N1 + 0x20, E2 = PoolNode(1) + 0x20;
    expect.Put(N1 + 0x38, 0, 8);
    expect.Put(N1 + 0x20, 0, 8);
    expect.Put(kSent + 0x10, E2, 8);
    expect.Put(E2 + 0x8, kSent, 8);
    expect.Put(N1 + 0x30, E1, 8);
    expect.Put(N1 + 0x28, E1, 8);
    expect.Put(kCnt, 3, 4);

    uint64_t node = 0;
    std::vector<SotcPcPoolNodes::Write> writes;
    Status st = Status::Unknown;
    const bool ok = PopNode(mem, kRoot, PreA(), &node, &writes, kOver, &st);
    CHECK(ok && st == Status::Popped);
    CHECK(node == N1);
    CHECK(writes.size() == 7);
    CHECK(SameImage(mem, expect));
    CHECK(mem.U64(kOver) == 0x500000);
    const PopPrediction pp = SotcPcPoolNodes::PredictPop(PreA(), 4);
    CHECK(pp.ok && !pp.refill && pp.count_post == 3 && pp.overhead_delta == 0);
    CHECK(SameSet(pp.post.set[0], PreA().set[0]));
}

// Each refused case: nothing is written, the image stays as it was, the status is Refused.
void ExpectRefused(const char* name, FakeMem& mem, uint64_t overhead, const Pools& pre) {
    FakeMem before = mem;
    uint64_t node = 0x55;
    std::vector<SotcPcPoolNodes::Write> writes;
    Status st = Status::Unknown;
    const bool ok = PopNode(mem, kRoot, pre, &node, &writes, overhead, &st);
    std::printf("  %s: %s\n", name, ok ? "POPPED (wrong)" : "refused");
    CHECK(!ok);
    CHECK(st == Status::Refused);
    CHECK(writes.empty());
    CHECK(node == 0);
    CHECK(SameImage(mem, before));
}

void TestRefusals() {
    std::printf("refused refills (nothing written):\n");
    // (a) S0 has no free run: the refill's request is Null.
    {
        FakeMem m;
        m.MapAll();
        Pools pre = MakePools(MakeSet(0, {}, 0, 0, 0x30000, 0x30000), Empty(1), Empty(2));
        BuildImage(&m, pre, 3, 3, false);
        ExpectRefused("no free run for the refill", m, kOver, pre);
    }
    // (b) S0's only run splits for the 64 KiB request (backed 128 KiB > request): not an exact hit.
    {
        FakeMem m;
        m.MapAll();
        Pools pre = MakePools(MakeSet(0, {Run{kFB, 2 * k64K, 2 * k64K, kNodeB}}, 0, 0, 0, 0), Empty(1), Empty(2));
        BuildImage(&m, pre, 3, 3, false);
        ExpectRefused("refill would split", m, kOver, pre);
    }
    // (c) No overhead counter address: the refill cannot be done faithfully.
    {
        FakeMem m;
        m.MapAll();
        BuildImage(&m, PreA(), 3, 3, false);
        ExpectRefused("refill without the counter address", m, 0, PreA());
    }
    // (d) The flexible block is not mapped: the carve probe refuses before anything is written.
    {
        FakeMem m;
        m.regions.clear();
        m.regions.push_back({kRoot, kRoot + 0x230000});
        m.regions.push_back({kNodeArea, kNodeArea + 0x10000000});
        BuildImage(&m, PreA(), 3, 3, false);
        ExpectRefused("unmapped refill block", m, kOver, PreA());
    }
    // (e) The pool list is empty (count 0): the game would dereference a null node.
    {
        FakeMem m;
        m.MapAll();
        BuildImage(&m, PreA(), 0, 0, false);
        ExpectRefused("empty pool list", m, kOver, PreA());
    }
    // (f) The list links do not agree (E2.prev names E3): refused before any write.
    {
        FakeMem m;
        m.MapAll();
        BuildImage(&m, PreA(), 3, 3, true);
        ExpectRefused("broken pool list link", m, kOver, PreA());
    }
}

void TestMutations() {
    std::printf("mutation checks (a wrong write or a failed write must be seen):\n");
    FakeMem pre;
    pre.MapAll();
    BuildImage(&pre, PreA(), 3, 3, false);
    FakeMem expect = pre;
    ExpectRefillA(expect);

    // Bit flip in the pop's P5 store (index 22 + 4 = 26): the image must differ from the hand transcription.
    {
        FakeMem inner = pre;
        MutMem mm;
        mm.inner = &inner;
        mm.flip_at = 26;
        uint64_t node = 0;
        Status st = Status::Unknown;
        PopNode(mm, kRoot, PreA(), &node, nullptr, kOver, &st);
        CHECK(!SameImage(inner, expect));
    }
    // Bit flip in the carve, C16 of the last node (index 30 + 584 * 17 + 15 = 9973): the image must differ. (A flip
    // of a store that a later node overwrites, such as C12 of a middle node, is not visible in the final image.)
    {
        FakeMem inner = pre;
        MutMem mm;
        mm.inner = &inner;
        mm.flip_at = 9973;
        uint64_t node = 0;
        Status st = Status::Unknown;
        PopNode(mm, kRoot, PreA(), &node, nullptr, kOver, &st);
        CHECK(!SameImage(inner, expect));
    }
    // A failed write in the pop (P4, index 25): the pop is partial, so the status is Unknown and nothing is claimed.
    {
        FakeMem inner = pre;
        MutMem mm;
        mm.inner = &inner;
        mm.fail_at = 25;
        uint64_t node = 0;
        Status st = Status::Popped;
        const bool ok = PopNode(mm, kRoot, PreA(), &node, nullptr, kOver, &st);
        CHECK(!ok);
        CHECK(st == Status::Unknown);
    }
    // The comparator itself: a one-byte change of the expected image must be seen.
    {
        FakeMem wrong = expect;
        wrong.Put(kFB + 584 * 0x70 + 0x69, 1, 1);
        CHECK(!SameImage(wrong, expect));
    }
}

void TestModelComposition() {
    std::printf("model: node-taking call with a refill:\n");
    // (1) Outer split on the second run, refill on the first: the composed post is the outer's decision on the
    // post-refill state.
    {
        Pools pre = MakePools(MakeSet(0,
                                      {Run{kFB, k64K, k64K, kNodeB}, Run{kFB + 0x100000, 3 * k64K, 3 * k64K, kNodeB + 0x100}},
                                      0, 0, 0x10000, 0x10000),
                              Empty(1), Empty(2));
        Request req;
        req.size = 2 * k64K;
        req.align = 8;
        req.kind = 1;
        Pools post;
        PopPrediction pop;
        const Prediction outer = SotcPcPoolNodes::PredictNodeCall(pre, req, nullptr, &post, 3, &pop);
        CHECK(outer.outcome == Outcome::Predicted);
        CHECK(outer.path == Path::FlexHitNode);
        CHECK(outer.slot == 0 && outer.va == kFB + 0x100000);
        CHECK(pop.ok && pop.refill && pop.count_post == 587);
        CHECK(post.set[0].alloc_bytes == pre.set[0].alloc_bytes + 3 * k64K); // outer 2 + refill 1 (64 KiB)
        bool first_gone = true;
        for (const Run& r : post.set[0].free_va) {
            if (r.base == kFB) {
                first_gone = false;
            }
        }
        CHECK(first_gone);
    }
    // (2) The refill takes the call's own run: the pad path of the call and the exact refill share one run.
    {
        Pools pre = MakePools(MakeSet(0, {Run{kFB + 0x10000, 3 * k64K, k64K, kNodeB}}, 0, 0, 0, 0), Empty(1), Empty(2));
        Request req;
        req.size = k64K;
        req.align = 2 * k64K;
        req.kind = 1;
        Pools post;
        PopPrediction pop;
        const Prediction outer = SotcPcPoolNodes::PredictNodeCall(pre, req, nullptr, &post, 3, &pop);
        CHECK(outer.outcome == Outcome::Unmodelled);
        CHECK(std::strcmp(outer.reason, "refill takes the call's own run") == 0);
        CHECK(!pop.ok);
    }
    // (3) A call that pops nothing (exact hit): the count stays and the post is Predict's.
    {
        Pools pre = PreA();
        Request req;
        req.size = k64K;
        req.align = 8;
        req.kind = 1;
        Pools post;
        PopPrediction pop;
        const Prediction outer = SotcPcPoolNodes::PredictNodeCall(pre, req, nullptr, &post, 3, &pop);
        CHECK(outer.path == Path::FlexHitExact);
        CHECK(pop.ok && !pop.refill && pop.count_post == 3);
    }
    // (4) An empty pool list refuses a call that would pop (a split: backed 128 KiB above the 64 KiB request).
    {
        Pools pre = MakePools(MakeSet(0, {Run{kFB, 2 * k64K, 2 * k64K, kNodeB}}, 0, 0, 0, 0), Empty(1), Empty(2));
        Request req;
        req.size = k64K;
        req.align = 8;
        req.kind = 1;
        Pools post;
        PopPrediction pop;
        const Prediction outer = SotcPcPoolNodes::PredictNodeCall(pre, req, nullptr, &post, 0, &pop);
        CHECK(outer.outcome == Outcome::Unmodelled);
        CHECK(!pop.ok);
    }
    // (5) PredictPop refuses a refill that is not an exact hit (the refused-refill case of PopNode).
    {
        Pools pre = MakePools(MakeSet(0, {Run{kFB, 2 * k64K, 2 * k64K, kNodeB}}, 0, 0, 0, 0), Empty(1), Empty(2));
        const PopPrediction pop = SotcPcPoolNodes::PredictPop(pre, 3);
        CHECK(!pop.ok && pop.refill);
    }
}

} // namespace

int main() {
    TestPopWithRefill();
    TestPopWithoutRefill();
    TestRefusals();
    TestMutations();
    TestModelComposition();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
