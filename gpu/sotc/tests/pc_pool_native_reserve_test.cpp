// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-native-reserve-test. Checks gpu/sotc/pc_pool_native_reserve.cpp (ApplyReserveMap, ApplyPoolFix)
// in three ways:
//   1. hand transcriptions: for a few fixed states the expected guest image is written out by hand (every
//      changed field, with its eboot store address in the comments of the .cpp), and the whole guest image
//      (every byte of the root, bitmap and node areas) is compared, together with the write list.
//   2. refusals: each refusal returns its status, writes nothing and leaves the image unchanged.
//   3. a differential fuzz against the model's ReserveMap (pc_pool.cpp, namespace Model): random states, the
//      status, the post set, the unmapped ranges and the guest bytes read back must agree.
// The POOL_FIX cases check the patched behaviour (same final image as the fresh path) and the latent-crash
// deviation (NextSet, nothing written, counted).
//
// Build and run (scratch directory, not the repo):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_native_reserve_test.cpp -o pc_pool_native_reserve_test.exe
//   ./pc_pool_native_reserve_test.exe   (prints PASS or FAIL; exit code 0 on PASS)
//
// This is a hand transcription, not a machine emulator: a shared misreading of the disassembly would pass both.
// The in-game comparison (the model's PC_POOL hooks) is the check of the real machine.

#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"
#include "../pc_pool_native_reserve.cpp"

#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

namespace M = SotcPcPool::Model;
namespace NR = SotcPcPoolNativeReserve;
using NR::ApplyPoolFix;
using NR::ApplyReserveMap;
using NR::ReserveResult;
using NR::Status;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);   \
        }                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        ++g_checks;                                                                                       \
        const auto va_ = (a);                                                                             \
        const auto vb_ = (b);                                                                             \
        if (!(va_ == vb_)) {                                                                              \
            ++g_failures;                                                                                 \
            std::printf("  FAIL line %d: %s == %s (got %llx, want %llx)\n", __LINE__, #a, #b,        \
                        static_cast<unsigned long long>(va_), static_cast<unsigned long long>(vb_));     \
        }                                                                                                 \
    } while (0)

constexpr uint64_t kR = 0x10000000ull;        // root (S0 = root, S1 = root+0xa0, S2 = root+0x140)
constexpr uint64_t kRootSize = 0x230000ull;
constexpr uint64_t kBitsBase = 0x50000000ull; // bitmap words: S1 here, S2 at +0x1000
constexpr uint64_t kNodeArea = 0x60000000ull;
constexpr uint64_t kOnion = 0x300000000ull;   // set 1 VA range
constexpr uint64_t kGarlic = 0x400000000ull;  // set 2 VA range
constexpr uint64_t k64K = 0x10000ull;
constexpr uint64_t k1M = 0x100000ull;
constexpr uint64_t kNoPeak = 1ull << 40;
const uint64_t kSetOff[3] = {0x000, 0x0a0, 0x140};

using Pair = std::pair<uint64_t, size_t>;

// ---------------------------------------------------------------------------------------------------
// Simulated guest memory: mapped ranges, read-only ranges, sparse bytes (unwritten bytes read as 0).
struct FakeMem : SotcPcPoolNative::GuestMem {
    std::vector<std::pair<uint64_t, uint64_t>> regions;
    std::vector<std::pair<uint64_t, uint64_t>> ro;
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
    bool ReadOnly(uint64_t a, size_t n) const {
        for (const auto& r : ro) {
            if (a < r.second && a + n > r.first) {
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
        if (!Mapped(a, n) || ReadOnly(a, n)) {
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
    int32_t I32(uint64_t a) const { return static_cast<int32_t>(static_cast<uint32_t>(U64(a) & 0xffffffffu)); }
    // Writes the low n bytes of v, little endian (no mapping check: the setup and the expected image use it).
    void Put(uint64_t a, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            bytes[a + static_cast<uint64_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    void InitMemory() {
        regions.clear();
        regions.push_back({kR, kR + kRootSize});
        regions.push_back({kBitsBase, kBitsBase + 0x10000});
        regions.push_back({kNodeArea, kNodeArea + 0x100000});
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

// ---------------------------------------------------------------------------------------------------
// Builds a world: the three sets in guest memory and in the model's Pools (the snapshot the call gets).
struct NodeSpec {
    uint64_t base;
    uint64_t size;
    uint64_t backed;
};

struct SetSpec {
    std::vector<NodeSpec> nodes; // free-VA list in list order
    std::vector<uint64_t> words; // bitmap (sets 1 and 2)
    uint32_t cap = 64;           // page cap
    int32_t cached = 0;          // +0x74
    int64_t free_pages = -1;     // +0x70: -1 = the sum of the node sizes in pages
};

uint64_t g_next_node = kNodeArea;

uint64_t NewNode() {
    const uint64_t n = g_next_node;
    g_next_node += 0x80;
    return n;
}

void BuildWorld(FakeMem& mem, M::Pools& pools, const SetSpec (&sp)[3], std::vector<uint64_t> (&nodes)[3]) {
    g_next_node = kNodeArea;
    mem.InitMemory();
    mem.bytes.clear();
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = kR + kSetOff[k];
        const SetSpec& s = sp[k];
        M::Set& ps = pools.set[k];
        ps = M::Set();
        ps.id = k;
        ps.has_bitmap = k != 0;
        ps.region_count = 0;
        ps.cached_phys = s.cached;
        ps.page_cap = k == 0 ? 0 : s.cap;
        uint64_t total = 0;
        for (const NodeSpec& nd : s.nodes) {
            total += nd.size;
        }
        ps.free_va_pages = s.free_pages < 0 ? static_cast<int32_t>(total / k64K) : static_cast<int32_t>(s.free_pages);
        ps.words = k == 0 ? std::vector<uint64_t>() : s.words;
        ps.words_addr = k == 0 ? 0 : kBitsBase + static_cast<uint64_t>(k - 1) * 0x1000;

        nodes[k].clear();
        for (const NodeSpec& nd : s.nodes) {
            const uint64_t N = NewNode();
            nodes[k].push_back(N);
            mem.Put(N + 0x50, nd.backed, 8);
            mem.Put(N + 0x58, nd.size, 8);
            mem.Put(N + 0x60, nd.base, 8);
            mem.Put(N + 0x68, static_cast<uint64_t>(k), 1);
            mem.Put(N + 0x69, 0, 1);
            ps.free_va.push_back(M::Run{nd.base, nd.size, nd.backed, N});
        }
        const uint64_t F = S + 0x28;
        const size_t n = nodes[k].size();
        for (size_t i = 0; i < n; ++i) {
            const uint64_t E = nodes[k][i] + 0x20;
            const uint64_t prev = i == 0 ? F : nodes[k][i - 1] + 0x20;
            const uint64_t next = i + 1 < n ? nodes[k][i + 1] + 0x20 : F;
            mem.Put(E, nodes[k][i], 8);
            mem.Put(E + 0x8, prev, 8);
            mem.Put(E + 0x10, next, 8);
        }
        const uint64_t head = n ? nodes[k][0] + 0x20 : F;
        const uint64_t tail = n ? nodes[k][n - 1] + 0x20 : F;
        mem.Put(S + 0x10, S, 8);            // region list empty (the sentinel points at itself)
        mem.Put(S + 0x20, 0, 4);            // region count
        mem.Put(S + 0x30, tail, 8);         // free-VA tail
        mem.Put(S + 0x38, head, 8);         // free-VA head
        mem.Put(S + 0x48, n, 4);            // free-VA count
        if (k > 0) {
            mem.Put(S + 0x50, ps.words_addr, 8);
            mem.Put(S + 0x58, ps.words.size(), 4);
            mem.Put(S + 0x60, ps.page_cap, 4);
            for (size_t w = 0; w < ps.words.size(); ++w) {
                mem.Put(ps.words_addr + 8 * w, ps.words[w], 8);
            }
        }
        mem.Put(S + 0x70, static_cast<uint64_t>(static_cast<uint32_t>(ps.free_va_pages)), 4);
        mem.Put(S + 0x74, static_cast<uint64_t>(static_cast<uint32_t>(ps.cached_phys)), 4);
        mem.Put(S + 0x78, static_cast<uint64_t>(k), 4);
        mem.Put(S + 0x80, 0, 8);
        mem.Put(S + 0x88, kNoPeak, 8);
        mem.Put(S + 0x90, 0, 8);
        mem.Put(S + 0x98, kNoPeak, 8);
        ps.stored_free_count = static_cast<int32_t>(n);
    }
    // Node pool (empty: sentinel R+0x1e0, head = sentinel, tail = sentinel), batch queue empty.
    mem.Put(kR + 0x1f0, kR + 0x1e0, 8);
    mem.Put(kR + 0x1e8, kR + 0x1e0, 8);
    mem.Put(kR + 0x200, 0, 4);
    mem.Put(kR + 0x2102f0, 0, 4);
}

// Empty lists in both sets, one zero bitmap word each (no free bit, no node).
void BuildEmpty(FakeMem& mem, M::Pools& pools, std::vector<uint64_t> (&nodes)[3]) {
    SetSpec sp[3];
    sp[1].words = {0};
    sp[2].words = {0};
    BuildWorld(mem, pools, sp, nodes);
}

// The set q as the guest holds it now, in the model's terms (for the read-back check).
M::Set ReadSet(FakeMem& mem, int q, size_t nwords) {
    M::Set s;
    const uint64_t S = kR + kSetOff[q];
    s.id = q;
    s.has_bitmap = true;
    s.region_count = mem.I32(S + 0x20);
    s.free_va_pages = mem.I32(S + 0x70);
    s.cached_phys = mem.I32(S + 0x74);
    s.stored_free_count = mem.I32(S + 0x48);
    s.alloc_bytes = mem.U64(S + 0x80);
    s.backed_bytes = mem.U64(S + 0x90);
    s.page_cap = static_cast<uint32_t>(mem.U64(S + 0x60) & 0xffffffffu);
    s.words_addr = mem.U64(S + 0x50);
    s.words.resize(nwords);
    for (size_t w = 0; w < nwords; ++w) {
        s.words[w] = mem.U64(s.words_addr + 8 * w);
    }
    const uint64_t F = S + 0x28;
    uint64_t e = mem.U64(S + 0x38);
    for (int guard = 0; e != F && guard < 10000; ++guard) {
        const uint64_t node = mem.U64(e);
        s.free_va.push_back(M::Run{mem.U64(node + 0x60), mem.U64(node + 0x58), mem.U64(node + 0x50), node});
        e = mem.U64(e + 0x10);
    }
    return s;
}

void CheckWrites(const std::vector<Pair>& want) {
    const std::vector<Pair> got = NR::LastWriteSet();
    CHECK_EQ(got.size(), want.size());
    if (got.size() == want.size()) {
        for (size_t i = 0; i < want.size(); ++i) {
            if (!(got[i] == want[i])) {
                ++g_failures;
                std::printf("  FAIL write %zu: got %llx/%zu want %llx/%zu\n", i,
                            static_cast<unsigned long long>(got[i].first), got[i].second,
                            static_cast<unsigned long long>(want[i].first), want[i].second);
            }
        }
    }
}

// The fixture of cases A, F and I: set 1 with one 4 MiB node, bitmap all zero, the even VA pages of its first
// 1 MiB mapped to phys bits 32, 34, ..., 46 (the odd pages are unmapped).
M::PhysMap MapEvenPages() {
    M::PhysMap map;
    for (int i = 0; i < 16; i += 2) {
        map.Map(kOnion + static_cast<uint64_t>(i) * k64K, k64K, static_cast<uint64_t>(32 + i));
    }
    return map;
}

void SetupA(FakeMem& mem, M::Pools& pre, std::vector<uint64_t> (&nodes)[3], int32_t cached) {
    SetSpec sp[3];
    sp[1].nodes = {{kOnion, 0x400000, 0x100000}};
    sp[1].words = {0};
    sp[1].cached = cached;
    BuildWorld(mem, pre, sp, nodes);
}

// ---------------------------------------------------------------------------------------------------
// Case A (hand transcription): set 1, node A (base kOnion, size 4 MiB, backed 1 MiB), bitmap 0, cached 0, rem 4 pages.
// want = 1 MiB (4 pages rounded to 1 MiB), len = 1 MiB, A is not used up. Map: 8 committed pages (bits 32..46 even).
// Expected by hand:
//   bitmap word 0 gets bits 32,34,36,38,40,42,44,46 = 0x0000555500000000
//   S1+0x74 cached = 8                     S1+0x70 free VA pages = 0x40 - 0x10 = 0x30
//   A+0x50 backed = 0x100000 - 8 * 64 KiB = 0x80000
//   queue entry 0: VA kOnion, phys 0, length 0x100000, prot 0, type 0, op 1; queue count = 1
//   A+0x60 base = kOnion + 0x100000        A+0x58 size = 0x300000
//   A+0x68 pool 1, A+0x69 in-use 0, A+0x40 owner 0, A+0x48 = -1
// Write list (eboot store order of 0x1abf0): R1 R2 R3 Q1..Q6 Q7 R4 P1..P6.
FakeMem g_caseA_image;

void CaseA() {
    std::printf("case A: partial consume with committed bits (hand transcription)\n");
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    SetupA(mem, pre, nodes, 0);
    const uint64_t A = nodes[1][0];
    const uint64_t S1 = kR + 0xa0;
    const uint64_t E0 = kR + 0x2102f8; // queue entry 0
    const M::PhysMap map = MapEvenPages();

    FakeMem exp = mem;
    exp.Put(kBitsBase, 0x0000555500000000ull, 8);
    exp.Put(S1 + 0x74, 8, 4);
    exp.Put(A + 0x50, 0x80000, 8);
    exp.Put(E0 + 0x00, kOnion, 8);
    exp.Put(E0 + 0x08, 0, 8);
    exp.Put(E0 + 0x10, 0x100000, 8);
    exp.Put(E0 + 0x18, 0, 1);
    exp.Put(E0 + 0x19, 0, 1);
    exp.Put(E0 + 0x1c, 1, 4);
    exp.Put(kR + 0x2102f0, 1, 4);
    exp.Put(S1 + 0x70, 0x30, 4);
    exp.Put(A + 0x68, 1, 1);
    exp.Put(A + 0x58, 0x300000, 8);
    exp.Put(A + 0x60, kOnion + 0x100000, 8);
    exp.Put(A + 0x69, 0, 1);
    exp.Put(A + 0x40, 0, 8);
    exp.Put(A + 0x48, 0xffffffffu, 4);
    g_caseA_image = exp;

    ReserveResult res;
    const Status st = ApplyReserveMap(mem, kR, pre, 1, 4, map, &res);
    CHECK(st == Status::Ok);
    CHECK(SameImage(mem, exp));
    CHECK_EQ(res.node_base, kOnion);
    CHECK_EQ(res.consumed, 0x100000ull);
    CHECK(!res.node_removed);
    CHECK_EQ(res.bits.size(), 8u);
    CHECK(res.bits.size() == 8 && res.bits[0] == 32 && res.bits[7] == 46);
    CHECK(res.unmapped.size() == 1 && res.unmapped[0].first == kOnion && res.unmapped[0].second == 0x100000);
    CheckWrites({{kBitsBase, 8}, {S1 + 0x74, 4}, {A + 0x50, 8}, {E0, 8}, {E0 + 8, 8}, {E0 + 0x10, 8},
                 {E0 + 0x18, 1}, {E0 + 0x19, 1}, {E0 + 0x1c, 4}, {kR + 0x2102f0, 4}, {S1 + 0x70, 4},
                 {A + 0x68, 1}, {A + 0x58, 8}, {A + 0x60, 8}, {A + 0x69, 1}, {A + 0x40, 8}, {A + 0x48, 4}});
    // The model agrees on the post set.
    const M::Set got = ReadSet(mem, 1, 1);
    CHECK(M::SameSet(got, res.post));
}

// ---------------------------------------------------------------------------------------------------
// Case B (hand transcription): set 2, three nodes X (1 MiB), Y (2 MiB), Z (2 MiB), no committed pages, rem 4.
// Every node has backed 0, so no node is above the remaining pages: SearchNode gives the first node, X.
// want = 1 MiB = X's size: X is used up. Head case of the unlink. X goes to the node pool.
void CaseB() {
    std::printf("case B: used-up node at the head of set 2, node pool push (hand transcription)\n");
    SetSpec sp[3];
    sp[2].nodes = {{kGarlic, k1M, 0}, {kGarlic + 0x200000, 0x200000, 0}, {kGarlic + 0x400000, 0x200000, 0}};
    sp[2].words = {0};
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    BuildWorld(mem, pre, sp, nodes);
    const uint64_t X = nodes[2][0], Y = nodes[2][1];
    const uint64_t S2 = kR + 0x140, F2 = S2 + 0x28;
    const uint64_t E0 = kR + 0x2102f8;
    const uint64_t pool_sentinel = kR + 0x1e0;

    FakeMem exp = mem;
    // Queue: entry 0 unmaps [kGarlic, +1 MiB), count 1.
    exp.Put(E0 + 0x00, kGarlic, 8);
    exp.Put(E0 + 0x08, 0, 8);
    exp.Put(E0 + 0x10, k1M, 8);
    exp.Put(E0 + 0x1c, 1, 4);
    exp.Put(kR + 0x2102f0, 1, 4);
    // Set 2: free VA pages 0x50 - 0x10 = 0x40, free count 3 - 1 = 2, head = Y.E.
    exp.Put(S2 + 0x70, 0x40, 4);
    exp.Put(S2 + 0x48, 2, 4);
    exp.Put(S2 + 0x38, Y + 0x20, 8);
    // Y's element: prev link = the set's sentinel F2 (X was the head).
    exp.Put(Y + 0x28, F2, 8);
    // X (used up): element links, flags, backed/size/base zeroed, count -1, owner list, node pool push.
    exp.Put(X + 0x20, X, 8);
    exp.Put(X + 0x28, pool_sentinel, 8);
    exp.Put(X + 0x30, pool_sentinel, 8); // U3 stored X.E, U12 overwrites it with the old pool head (the sentinel)
    exp.Put(X + 0x38, pool_sentinel, 8);
    exp.Put(X + 0x40, 0, 8);
    exp.Put(X + 0x48, 0xffffffffu, 4);
    exp.Put(X + 0x50, 0, 8);
    exp.Put(X + 0x58, 0, 8);
    exp.Put(X + 0x60, 0, 8);
    exp.Put(X + 0x69, 0, 1);
    exp.Put(kR + 0x1f0, X + 0x20, 8);     // node pool head
    exp.Put(pool_sentinel + 0x8, X + 0x20, 8); // old head (the sentinel) + 8
    exp.Put(kR + 0x200, 1, 4);             // node pool count

    ReserveResult res;
    const Status st = ApplyReserveMap(mem, kR, pre, 2, 4, M::PhysMap(), &res);
    CHECK(st == Status::Ok);
    CHECK(SameImage(mem, exp));
    CHECK_EQ(res.node_base, kGarlic);
    CHECK(res.node_removed);
    CHECK(res.bits.empty());
    CheckWrites({{E0, 8}, {E0 + 8, 8}, {E0 + 0x10, 8}, {E0 + 0x18, 1}, {E0 + 0x19, 1}, {E0 + 0x1c, 4},
                 {kR + 0x2102f0, 4}, {S2 + 0x70, 4},
                 {S2 + 0x38, 8},   // U1: [F2+0x10] = next
                 {Y + 0x28, 8},    // U2: [Y.E+8] = F2
                 {X + 0x30, 8},    // U3: dead store
                 {S2 + 0x48, 4},   // U4
                 {X + 0x69, 1},    // U5
                 {X + 0x40, 8},    // U6
                 {X + 0x50, 16},   // U7
                 {X + 0x60, 8},    // U8
                 {X + 0x48, 4},    // U9
                 {X + 0x20, 8},    // U10
                 {X + 0x28, 8},    // U11
                 {X + 0x30, 8},    // U12
                 {kR + 0x1f0, 8},  // U13
                 {pool_sentinel + 8, 8}, // U14
                 {kR + 0x200, 4},  // U15
                 {X + 0x38, 8}});  // U16
    const M::Set got = ReadSet(mem, 2, 1);
    CHECK_EQ(got.free_va.size(), 2u);
    CHECK(got.free_va.size() == 2 && got.free_va[0].base == kGarlic + 0x200000);
}

// ---------------------------------------------------------------------------------------------------
// Case C (sticky quirk): set 1, A (5 backed pages), B (9), C (6), each 2 MiB, rem 4. The model's SearchNode gives
// C: A is the first candidate, B replaces it (best == 0 holds), C beats B. A true best fit would give A (5).
// want = 1 MiB < 2 MiB: C is not used up, its base and size move. No map: no bits, no backed write.
void CaseC() {
    std::printf("case C: sticky first-candidate quirk picks C, not the best fit A (hand transcription)\n");
    SetSpec sp[3];
    sp[1].nodes = {{kOnion, 0x200000, 5 * k64K}, {kOnion + 0x400000, 0x200000, 9 * k64K},
                   {kOnion + 0x800000, 0x200000, 6 * k64K}};
    sp[1].words = {0};
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    BuildWorld(mem, pre, sp, nodes);
    const uint64_t C = nodes[1][2];
    const uint64_t S1 = kR + 0xa0;
    const uint64_t E0 = kR + 0x2102f8;

    FakeMem exp = mem;
    exp.Put(E0 + 0x00, kOnion + 0x800000, 8);
    exp.Put(E0 + 0x10, k1M, 8);
    exp.Put(E0 + 0x1c, 1, 4);
    exp.Put(kR + 0x2102f0, 1, 4);
    exp.Put(S1 + 0x70, 0x50, 4); // 96 - 16
    exp.Put(C + 0x60, kOnion + 0x800000 + k1M, 8);
    exp.Put(C + 0x58, k1M, 8);
    exp.Put(C + 0x69, 0, 1);
    exp.Put(C + 0x40, 0, 8);
    exp.Put(C + 0x48, 0xffffffffu, 4);

    ReserveResult res;
    const Status st = ApplyReserveMap(mem, kR, pre, 1, 4, M::PhysMap(), &res);
    CHECK(st == Status::Ok);
    CHECK(SameImage(mem, exp));
    CHECK_EQ(res.node_base, kOnion + 0x800000);
    CHECK(!res.node_removed);
    CHECK(res.unmapped.size() == 1 && res.unmapped[0].first == kOnion + 0x800000 && res.unmapped[0].second == k1M);
    CheckWrites({{E0, 8}, {E0 + 8, 8}, {E0 + 0x10, 8}, {E0 + 0x18, 1}, {E0 + 0x19, 1}, {E0 + 0x1c, 4},
                 {kR + 0x2102f0, 4}, {S1 + 0x70, 4}, {C + 0x68, 1}, {C + 0x58, 8}, {C + 0x60, 8},
                 {C + 0x69, 1}, {C + 0x40, 8}, {C + 0x48, 4}});
}

// ---------------------------------------------------------------------------------------------------
// Case D (exact first match on the sticky scan): set 1, A (2 pages), B (4 pages, equal to rem), C (9 pages).
// The scan returns B at once. want = 1 MiB < 2 MiB: B keeps part of its VA.
void CaseD() {
    std::printf("case D: a node whose backed pages equal the remaining pages is taken at once\n");
    SetSpec sp[3];
    sp[1].nodes = {{kOnion, 0x200000, 2 * k64K}, {kOnion + 0x400000, 0x200000, 4 * k64K},
                   {kOnion + 0x800000, 0x200000, 9 * k64K}};
    sp[1].words = {0};
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    BuildWorld(mem, pre, sp, nodes);
    const uint64_t B = nodes[1][1];
    const uint64_t S1 = kR + 0xa0;
    const uint64_t E0 = kR + 0x2102f8;

    FakeMem exp = mem;
    exp.Put(E0 + 0x00, kOnion + 0x400000, 8);
    exp.Put(E0 + 0x10, k1M, 8);
    exp.Put(E0 + 0x1c, 1, 4);
    exp.Put(kR + 0x2102f0, 1, 4);
    exp.Put(S1 + 0x70, 0x50, 4);
    exp.Put(B + 0x60, kOnion + 0x400000 + k1M, 8);
    exp.Put(B + 0x58, k1M, 8);
    exp.Put(B + 0x69, 0, 1);
    exp.Put(B + 0x40, 0, 8);
    exp.Put(B + 0x48, 0xffffffffu, 4);

    ReserveResult res;
    const Status st = ApplyReserveMap(mem, kR, pre, 1, 4, M::PhysMap(), &res);
    CHECK(st == Status::Ok);
    CHECK(SameImage(mem, exp));
    CHECK_EQ(res.node_base, kOnion + 0x400000);
}

// ---------------------------------------------------------------------------------------------------
// Case E: refusals. Each one returns its status, writes nothing and leaves the image unchanged.
void Refusal(const char* name, Status want, FakeMem& mem, const M::Pools& pre, int q, uint64_t rem,
             const M::PhysMap& map, bool pool_fix) {
    const FakeMem before = mem;
    ReserveResult res;
    const Status st = pool_fix ? ApplyPoolFix(mem, kR, pre, q, rem, map, &res) : ApplyReserveMap(mem, kR, pre, q, rem, map, &res);
    std::printf("  %s: %s\n", name, NR::StatusName(st));
    CHECK(st == want);
    CHECK(SameImage(mem, before));
    CHECK(NR::LastWriteSet().empty());
}

void RefusalCases() {
    std::printf("case E: refusals write nothing\n");
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 3);
        Refusal("cached != 0 on the fresh path", Status::NotApplicable, mem, pre, 1, 4, MapEvenPages(), false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        M::PhysMap map = MapEvenPages();
        map.MarkUnknown(kOnion + 0x800000, k64K); // outside the consumed range: still refused (global rule)
        Refusal("unknown range in the map", Status::UnknownMap, mem, pre, 1, 4, map, false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        Refusal("set index 0 (no bitmap)", Status::NotApplicable, mem, pre, 0, 4, MapEvenPages(), false);
        Refusal("rem 0", Status::NotApplicable, mem, pre, 1, 0, MapEvenPages(), false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        mem.Put(kR + 0xa0 + 0x70, 0x31, 4); // guest free VA pages differ from the snapshot
        Refusal("guest header differs from the snapshot", Status::Refused, mem, pre, 1, 4, MapEvenPages(), false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        mem.Put(kBitsBase, 1, 8); // guest bitmap differs from the snapshot
        Refusal("guest bitmap differs from the snapshot", Status::Refused, mem, pre, 1, 4, MapEvenPages(), false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        M::PhysMap map;
        map.Map(kOnion, k64K, 64); // bit 64 is past the one-word bitmap
        Refusal("mapped bit outside the bitmap", Status::Refused, mem, pre, 1, 4, map, false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        BuildEmpty(mem, pre, nodes);
        Refusal("empty free-VA list (ApplyReserveMap)", Status::NoNode, mem, pre, 1, 4, M::PhysMap(), false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        mem.Put(kR + 0x2102f0, 64, 4); // the batch queue is full
        Refusal("batch queue full", Status::QueueFull, mem, pre, 1, 4, MapEvenPages(), false);
    }
    {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        mem.Put(kR + 0x1f0, 0, 8); // node pool head null
        Refusal("node pool head null", Status::Refused, mem, pre, 1, 4, MapEvenPages(), false);
    }
    {
        // A failing write in the middle: the page of the set's free pages is read-only. The earlier writes
        // (bitmap, cached, backed, queue) stay; the status says so.
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetupA(mem, pre, nodes, 0);
        mem.ro.push_back({kR + 0xa0 + 0x70, kR + 0xa0 + 0x74});
        const M::PhysMap map = MapEvenPages();
        ReserveResult res;
        const Status st = ApplyReserveMap(mem, kR, pre, 1, 4, map, &res);
        std::printf("  write refused mid-sequence: %s\n", NR::StatusName(st));
        CHECK(st == Status::WriteFailed);
        CHECK_EQ(NR::LastWriteSet().size(), 10u); // R1, R2, R3, Q1..Q6, Q7
        CHECK_EQ(mem.U64(kBitsBase), 0x0000555500000000ull);
        CHECK_EQ(mem.I32(kR + 0xa0 + 0x74), 8);
        CHECK_EQ(mem.I32(kR + 0x2102f0), 1);
        CHECK_EQ(mem.I32(kR + 0xa0 + 0x70), 0x40); // not written: the failing store
    }
}

// ---------------------------------------------------------------------------------------------------
// Case F (POOL_FIX): the patched behaviour on the same state as case A. The patch zeroes cached (here 5) and
// takes the same step, so the final image is case A's, and the first store is the zeroing.
void CaseF() {
    std::printf("case F: POOL_FIX with cached > 0 and no free bit, same final image as the fresh path\n");
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    SetupA(mem, pre, nodes, 5);
    ReserveResult res;
    NR::ResetCounters();
    const Status st = ApplyPoolFix(mem, kR, pre, 1, 4, MapEvenPages(), &res);
    CHECK(st == Status::Ok);
    CHECK(SameImage(mem, g_caseA_image));
    const std::vector<Pair> writes = NR::LastWriteSet();
    CHECK(!writes.empty() && writes[0] == Pair(kR + 0xa0 + 0x74, 4)); // mov dword [r14+0x74], 0 comes first
    CHECK_EQ(writes.size(), 18u);
    CHECK_EQ(NR::Counters().poolfix_ok, 1u);
    CHECK_EQ(NR::Counters().poolfix_next_set, 0u);
}

// POOL_FIX with cached already 0 and a list: the same final image.
void CaseF2() {
    std::printf("case F2: POOL_FIX with cached 0 and a non-empty list\n");
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    SetupA(mem, pre, nodes, 0);
    ReserveResult res;
    const Status st = ApplyPoolFix(mem, kR, pre, 1, 4, MapEvenPages(), &res);
    CHECK(st == Status::Ok);
    CHECK(SameImage(mem, g_caseA_image));
}

// ---------------------------------------------------------------------------------------------------
// Case G (POOL_FIX latent crash): no free bit AND an empty free-VA list. The patched game would read [0+0x58];
// the native path writes nothing, returns NextSet and counts it. Both cached values.
void CaseG() {
    std::printf("case G: POOL_FIX latent crash (no free bit, empty list): NextSet, nothing written, counted\n");
    for (int32_t cached : {5, 0}) {
        FakeMem mem;
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        SetSpec sp[3]; // set 1: no node, one zero bitmap word (no free bit), cached as given
        sp[1].words = {0};
        sp[1].cached = cached;
        BuildWorld(mem, pre, sp, nodes);
        NR::ResetCounters();
        ReserveResult res;
        const FakeMem before = mem;
        const Status st = ApplyPoolFix(mem, kR, pre, 1, 4, MapEvenPages(), &res);
        CHECK(st == Status::NextSet);
        CHECK(SameImage(mem, before));
        CHECK(NR::LastWriteSet().empty());
        CHECK_EQ(NR::Counters().poolfix_next_set, 1u);
    }
    // The same state through ApplyReserveMap (cached 0) is the model's Crash: NoNode, not NextSet.
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    BuildEmpty(mem, pre, nodes);
    ReserveResult res;
    CHECK(ApplyReserveMap(mem, kR, pre, 1, 4, M::PhysMap(), &res) == Status::NoNode);
}

// POOL_FIX with a free bit present is not the patch's case.
void CaseH() {
    std::printf("case H: POOL_FIX refuses a state with a free bit\n");
    SetSpec sp[3];
    sp[1].nodes = {{kOnion, 0x400000, 0x100000}};
    sp[1].words = {0x20}; // bit 5 is free
    sp[1].cached = 0;
    FakeMem mem;
    M::Pools pre;
    std::vector<uint64_t> nodes[3];
    BuildWorld(mem, pre, sp, nodes);
    NR::ResetCounters();
    ReserveResult res;
    const FakeMem before = mem;
    CHECK(ApplyPoolFix(mem, kR, pre, 1, 4, M::PhysMap(), &res) == Status::NotApplicable);
    CHECK(SameImage(mem, before));
    CHECK_EQ(NR::Counters().poolfix_not_applicable, 1u);
}

// ---------------------------------------------------------------------------------------------------
// Case R: random states against the model's ReserveMap (pc_pool.cpp). The status, the post set, the unmapped
// ranges, and the guest bytes read back must agree, and the other set must not change.
void Fuzz() {
    std::printf("case R: differential fuzz against Model::ReserveMap\n");
    std::mt19937_64 rng(20261101u);
    auto rnd = [&](uint64_t n) { return n == 0 ? 0 : rng() % n; };
    int ok_count = 0, crash_count = 0, bad = 0;
    const int kIters = 3000;
    for (int iter = 0; iter < kIters; ++iter) {
        const int q = 1 + static_cast<int>(rnd(2));
        SetSpec sp[3];
        for (int k = 1; k <= 2; ++k) {
            const uint64_t base_va = k == 1 ? kOnion : kGarlic;
            const uint64_t n = (k == q) ? rnd(6) : rnd(3);
            uint64_t cur = base_va;
            for (uint64_t i = 0; i < n; ++i) {
                cur += rnd(4) * k64K;
                const uint64_t size = (1 + rnd(40)) * k64K;
                const uint64_t backed = rnd(size / k64K + 1) * k64K;
                sp[k].nodes.push_back({cur, size, backed});
                cur += size;
            }
            sp[k].words.resize(2);
            for (auto& w : sp[k].words) {
                w = rng() & rng(); // about a third of the bits free
            }
            sp[k].cap = 128;
            sp[k].cached = 0;
        }
        FakeMem mem;
        mem.InitMemory();
        M::Pools pre;
        std::vector<uint64_t> nodes[3];
        BuildWorld(mem, pre, sp, nodes);

        // The VA -> phys map: random committed pages in the set's node ranges.
        M::PhysMap map;
        for (const NodeSpec& nd : sp[q].nodes) {
            for (uint64_t off = 0; off < nd.size; off += k64K) {
                if (rnd(2) == 0) {
                    map.Map(nd.base + off, k64K, rnd(128));
                }
            }
        }
        const uint64_t rem = 1 + rnd(24);

        M::Pools mpost = pre;
        M::Prediction mp;
        const M::RmStatus ms = M::ReserveMap(&mpost, q, &map, rem, &mp);

        const FakeMem before = mem;
        ReserveResult res;
        const Status st = ApplyReserveMap(mem, kR, pre, q, rem, map, &res);
        if (ms == M::RmStatus::Ok) {
            ++ok_count;
            bool good = st == Status::Ok && M::SameSet(res.post, mpost.set[q]) && res.unmapped.size() == 1 &&
                        mp.unmapped.size() == 1;
            if (good) {
                good = res.unmapped[0].first == mp.unmapped[0].base && res.unmapped[0].second == mp.unmapped[0].size;
            }
            const M::Set guest = ReadSet(mem, q, pre.set[q].words.size());
            good = good && M::SameSet(guest, mpost.set[q]);
            const int other = 3 - q;
            good = good && M::SameSet(ReadSet(mem, other, pre.set[other].words.size()), pre.set[other]);
            if (!good) {
                ++bad;
                if (bad <= 5) {
                    std::printf("  fuzz iter %d: status %s, model Ok (q %d, rem %llu)\n", iter, NR::StatusName(st), q,
                                static_cast<unsigned long long>(rem));
                }
            }
        } else if (ms == M::RmStatus::Crash) {
            ++crash_count;
            const bool good = st == Status::NoNode && SameImage(mem, before);
            if (!good) {
                ++bad;
                if (bad <= 5) {
                    std::printf("  fuzz iter %d: model Crash, native %s\n", iter, NR::StatusName(st));
                }
            }
        } else {
            ++bad;
            std::printf("  fuzz iter %d: unexpected model status %d\n", iter, static_cast<int>(ms));
        }
    }
    std::printf("  %d iterations: %d Ok, %d empty-list, %d disagreements\n", kIters, ok_count, crash_count, bad);
    CHECK(bad == 0);
    CHECK(ok_count > 500);
    CHECK(crash_count > 0);
}

} // namespace

int main() {
    CaseA();
    CaseB();
    CaseC();
    CaseD();
    RefusalCases();
    CaseF();
    CaseF2();
    CaseG();
    CaseH();
    Fuzz();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures == 0 ? "PASS\n" : "FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
