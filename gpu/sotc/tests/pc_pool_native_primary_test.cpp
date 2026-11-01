// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-native-primary-test. Checks gpu/sotc/pc_pool_native_primary.cpp (ApplyPrimaryNodeHit, the pad and
// split hits of the primary sets, with their node pops and refills) against a second, independent transcription of
// the same path (Ref below), written as the disassembly reads: labels for the walk, pops, refill and tails, reads at
// the point the game reads them. Both run on copies of one simulated guest memory. The test checks, per scenario:
//   - Ref refuses (a refill that is not an exact hit or takes the call's own run, or the path is not a node hit): the
//     production returns 0 and writes nothing and the memory is unchanged;
//   - otherwise the return value, every byte of the memory, and the write list (address and size of each Write).
//     The production performs a refill's nested exact hit before the pop stores (the overlay stages both), so the
//     write lists are compared as multisets when a refill ran; the memory is compared byte for byte in every case.
// Directed cases cover: a refill on the pad's pop (count 3), on the split's pop (count 3), on the second pop of a
// pad-and-split (count 4, not modelled by PredictNodeCall, native here), refusals (no exact run for the refill, an
// empty pool, an unreadable owner family, a fabricated exact hit, a wrong start).
//
// Build and run (scratch directory, not the repo). The node pops need pc_pool_native_nodes.cpp, and the refill its
// exact hit (pc_pool_native.cpp); they are separate translation units, because both files define the same internal
// helper names in their namespace:
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_native_primary_test.cpp
//     gpu/sotc/pc_pool_native.cpp -o prim_test.exe
//   ./prim_test.exe   (prints PASS or FAIL; exit code 0 on PASS)
// For the mutation check, define PRIMARY_SRC to a mutated copy of pc_pool_native_primary.cpp:
//   -DPRIMARY_SRC=\"C:/.../mut.cpp\"  (the copy must keep its quoted includes resolvable through -I gpu/sotc)
//
// The reference is a hand transcription, not a machine emulator: a shared misreading of the disassembly passes both.
// The in-game comparison (the model's PC_POOL hooks) is the check of the real machine.

#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"

#ifdef PRIMARY_SRC
#include PRIMARY_SRC
#else
#include "../pc_pool_native_primary.cpp"
#endif

#include "../pc_pool_native_nodes.cpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace SotcPcPool::Model;
using SotcPcPoolNative::ApplyPrimaryNodeHit;
using SotcPcPoolNative::GuestMem;
using SotcPcPoolNative::PrimaryLastWriteSet;

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

// The pool root sits far from the heap tables below (the writers refuse node pointers inside the root struct, see
// InPoolRoot): the root struct is mapped on its own.
constexpr uint64_t kRoot = 0x10000000;
constexpr uint64_t kMemSize = 0x2c0000;      // the image: every address below it is mapped
constexpr uint64_t kNodePoolArea = 0x180000; // pool nodes N_i = kNodePoolArea + i * 0x70
constexpr uint64_t kFreeNodeArea = 0x200000; // free-VA nodes
constexpr uint64_t kRegionArea = 0x270000;   // region list elements
constexpr uint64_t kObj = 0x2b0000;          // owner family (readable)
constexpr uint64_t kObjBad = 0x2d0000;       // beyond the image: unmapped, unreadable
constexpr uint64_t kOverhead = 0x2b8000;     // DAT_02dfca60 (the refill's bookkeeping qword), mapped
constexpr uint64_t kOverheadInit = 0x1000;
constexpr uint64_t kRefill = ~uint64_t{0};   // Ref: the pop refuses (refill not exact, or the call's own run)
constexpr uint64_t k64K = 0x10000;
constexpr uint64_t k1M = 0x100000;

// Simulated guest memory: sparse bytes (unwritten mapped bytes read as 0) over a set of mapped ranges. The image
// [0, kMemSize) is mapped; each free run of a scenario is mapped too (a refill's block is one of them).
struct Mem : GuestMem {
    std::map<uint64_t, uint8_t> bytes;
    std::vector<std::pair<uint64_t, uint64_t>> mapped; // [lo, hi)

    bool IsMapped(uint64_t a, size_t n) const {
        for (const auto& r : mapped) {
            if (a >= r.first && n <= r.second && a - r.first <= r.second - n) {
                return true;
            }
        }
        return false;
    }
    uint8_t Get(uint64_t a) const {
        const auto it = bytes.find(a);
        return it == bytes.end() ? 0 : it->second;
    }
    bool Read(uint64_t a, void* o, size_t n) override {
        if (!IsMapped(a, n)) {
            return false;
        }
        auto* out = static_cast<uint8_t*>(o);
        for (size_t i = 0; i < n; ++i) {
            out[i] = Get(a + i);
        }
        return true;
    }
    bool Write(uint64_t a, const void* in, size_t n) override {
        if (!IsMapped(a, n)) {
            return false;
        }
        const auto* p = static_cast<const uint8_t*>(in);
        for (size_t i = 0; i < n; ++i) {
            bytes[a + i] = p[i];
        }
        return true;
    }
    void Clear() {
        bytes.clear();
        mapped.clear();
        mapped.push_back({0, kMemSize});
        mapped.push_back({kRoot, kRoot + 0x230000}); // the pool root struct
    }
};

// True when both memories hold the same byte at every address (an unwritten byte is 0).
bool SameMem(const Mem& a, const Mem& b) {
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

uint64_t R64(Mem& m, uint64_t a) {
    uint64_t v = 0;
    m.Read(a, &v, 8);
    return v;
}
int32_t R32(Mem& m, uint64_t a) {
    int32_t v = 0;
    m.Read(a, &v, 4);
    return v;
}
uint8_t R8(Mem& m, uint64_t a) {
    uint8_t v = 0;
    m.Read(a, &v, 1);
    return v;
}
void P64(Mem& m, uint64_t a, uint64_t v) { m.Write(a, &v, 8); }
void P32(Mem& m, uint64_t a, int32_t v) { m.Write(a, &v, 4); }
void P8(Mem& m, uint64_t a, uint8_t v) { m.Write(a, &v, 1); }

// ---------------------------------------------------------------------------------------------------
// Scenario: one memory image and the model's Pools built from the same numbers.
struct RunSpec {
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t backed = 0;
};

struct SetSpec {
    std::vector<RunSpec> runs;
    int regions = 0;
    int32_t cached = 0;
    uint64_t alloc = 0;
    uint64_t backed = 0;
    uint64_t alloc_peak = 0;
    uint64_t backed_peak = 0;
};

struct Scenario {
    SetSpec set[3];
    int node_count = 0; // the node pool: count = list length
    uint64_t size = 0;
    uint32_t align = 0;
    int kind = 1;
    uint64_t obj = 0;
    uint8_t tag = 0;
};

uint64_t SetBase(int k) { return kRoot + (k == 0 ? 0x000 : k == 1 ? 0x0a0 : 0x140); }

// Writes the image of one scenario (cleared first) and returns the model's Pools for it.
Pools Build(Mem& m, const Scenario& sc) {
    m.Clear();
    Pools pools;
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = SetBase(k);
        const SetSpec& ss = sc.set[k];
        Set& ps = pools.set[k];
        ps.id = k;
        ps.has_bitmap = k != 0;
        ps.region_count = ss.regions;
        ps.cached_phys = ss.cached;
        ps.alloc_bytes = ss.alloc;
        ps.backed_bytes = ss.backed;
        // Region list: ss.regions elements, circular through S (head S+0x10, tail S+8, element next at +0x10).
        std::vector<uint64_t> regs;
        for (int r = 0; r < ss.regions; ++r) {
            regs.push_back(kRegionArea + static_cast<uint64_t>(k * 8 + r) * 0x20);
        }
        for (size_t i = 0; i < regs.size(); ++i) {
            const uint64_t next = i + 1 < regs.size() ? regs[i + 1] : S;
            const uint64_t prev = i == 0 ? S : regs[i - 1];
            P64(m, regs[i] + 0x10, next);
            P64(m, regs[i] + 0x8, prev);
        }
        P64(m, S + 0x10, regs.empty() ? S : regs.front());
        P64(m, S + 0x8, regs.empty() ? S : regs.back());
        P32(m, S + 0x20, ss.regions);
        // Free-VA list: nodes N_j at kFreeNodeArea + (k*32 + j) * 0x80, element E = N + 0x20, sentinel F = S + 0x28.
        const uint64_t F = S + 0x28;
        std::vector<uint64_t> elems;
        uint64_t pages = 0;
        for (size_t j = 0; j < ss.runs.size(); ++j) {
            const RunSpec& rs = ss.runs[j];
            const uint64_t N = kFreeNodeArea + static_cast<uint64_t>(k * 32 + static_cast<int>(j)) * 0x80;
            P64(m, N + 0x60, rs.base);
            P64(m, N + 0x58, rs.size);
            P64(m, N + 0x50, rs.backed);
            P8(m, N + 0x68, static_cast<uint8_t>(k));
            P8(m, N + 0x69, 1);
            P64(m, N + 0x40, 0);
            P32(m, N + 0x48, -1);
            P64(m, N + 0x20, N); // E + 0 = N
            elems.push_back(N + 0x20);
            ps.free_va.push_back(Run{rs.base, rs.size, rs.backed, N});
            pages += rs.size / k64K;
            m.mapped.push_back({rs.base, rs.base + rs.size});
        }
        for (size_t j = 0; j < elems.size(); ++j) {
            const uint64_t E = elems[j];
            P64(m, E + 0x8, j == 0 ? F : elems[j - 1]);                // prev
            P64(m, E + 0x10, j + 1 < elems.size() ? elems[j + 1] : F); // next
        }
        P64(m, S + 0x38, elems.empty() ? F : elems.front()); // head
        P64(m, S + 0x30, elems.empty() ? F : elems.back());  // tail
        P32(m, S + 0x48, static_cast<int32_t>(elems.size()));
        P32(m, S + 0x70, static_cast<int32_t>(pages));
        P32(m, S + 0x74, ss.cached);
        P32(m, S + 0x78, k);
        P64(m, S + 0x80, ss.alloc);
        P64(m, S + 0x88, ss.alloc_peak);
        P64(m, S + 0x90, ss.backed);
        P64(m, S + 0x98, ss.backed_peak);
        ps.free_va_pages = static_cast<int32_t>(pages);
        ps.stored_free_count = static_cast<int32_t>(elems.size());
    }
    // Node pool: list of node_count nodes, head root+0x1f0, tail root+0x1e8, count root+0x200.
    const uint64_t F = kRoot + 0x1e0;
    std::vector<uint64_t> nodes;
    for (int i = 0; i < sc.node_count; ++i) {
        const uint64_t N = kNodePoolArea + static_cast<uint64_t>(i) * 0x70;
        nodes.push_back(N + 0x20);
        P64(m, N + 0x20, N);
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        const uint64_t E = nodes[i];
        P64(m, E + 0x8, i == 0 ? F : nodes[i - 1]);
        P64(m, E + 0x10, i + 1 < nodes.size() ? nodes[i + 1] : F);
    }
    P64(m, kRoot + 0x1f0, nodes.empty() ? F : nodes.front());
    P64(m, kRoot + 0x1e8, nodes.empty() ? F : nodes.back());
    P32(m, kRoot + 0x200, static_cast<int32_t>(nodes.size()));
    P64(m, kRoot + 0x20d908, 0); // the flexible base (not used by the primary path)
    P64(m, kOverhead, kOverheadInit);
    // Owner family.
    P8(m, kObj + 0x1c, sc.tag);
    return pools;
}

// ---------------------------------------------------------------------------------------------------
// Reference: the original path, transcribed from the disassembly. Every Write is logged as (address, size).
using Log = std::vector<std::pair<uint64_t, size_t>>;

struct Ref {
    Mem& m;
    Log log;
    uint64_t keep_lo = 0; // the call's own run (its refill must not take it)
    uint64_t keep_hi = 0;
    bool refilled = false;
    explicit Ref(Mem& mm) : m(mm) {}
    void W(uint64_t a, uint64_t v, size_t n) {
        m.Write(a, &v, n);
        log.push_back({a, n});
    }
    void W64(uint64_t a, uint64_t v) { W(a, v, 8); }
    void W32(uint64_t a, int32_t v) { W(a, static_cast<uint32_t>(v), 4); }
    void W8(uint64_t a, uint8_t v) { W(a, v, 1); }
    void Inc32(uint64_t a) { W32(a, R32(m, a) + 1); }
    void Dec32(uint64_t a) { W32(a, R32(m, a) - 1); }

    // First fit of set S (0x1a86e-0x1a8a9) for a run of bytes with alignment aarg: true when some node fits.
    // Walks the list as the code does: r12 = head (0 at the sentinel), size, aligned start, next = [[r12+0x30]].
    bool Fit(uint64_t S, uint64_t size, uint64_t aarg, uint64_t* node, uint64_t* pad) {
        const uint64_t F = S + 0x28;
        const uint64_t head = R64(m, S + 0x38); // an element pointer; its node is [head]
        uint64_t r12 = head == F ? 0 : R64(m, head);
        while (r12 != 0) {
            if (R64(m, r12 + 0x58) >= size) {
                const uint64_t base = R64(m, r12 + 0x60);
                const uint64_t p = ((base + aarg - 1) & ~(aarg - 1)) - base;
                if (p == 0 || p + size <= R64(m, r12 + 0x58)) {
                    *node = r12;
                    *pad = p;
                    return true;
                }
            }
            r12 = R64(m, R64(m, r12 + 0x30));
        }
        return false;
    }

    // pool_node_alloc's pop (0x19660, 0x1968b-0x196c3), then its refill when the count goes 3 -> 2 (0x196c9-0x19722):
    // the bookkeeping add, the nested exact hit (pool_alloc_region 0x19e00 with 64 KiB, align 8, kind 1), the carve of
    // 585 nodes. Returns the node, 0 on an empty pool, kRefill when the refill is refused.
    uint64_t Pop(uint64_t root) {
        const uint64_t F = root + 0x1e0;
        const uint64_t head = R64(m, root + 0x1f0);
        if (head == F) {
            return 0;
        }
        const uint64_t N = R64(m, head);
        const uint64_t nx = R64(m, N + 0x30);
        W64(N + 0x38, 0);
        W64(N + 0x20, 0);
        const uint64_t pv = R64(m, N + 0x28);
        W64(pv + 0x10, nx);
        const uint64_t nx2 = R64(m, N + 0x30);
        W64(nx2 + 0x8, pv);
        W64(N + 0x30, N + 0x20);
        W64(N + 0x28, N + 0x20);
        const int32_t c = R32(m, root + 0x200) - 1;
        W32(root + 0x200, c);
        if (c != 2) {
            return N;
        }
        // 0x196c9-0x196cc: the refill. R0: the bookkeeping add (the game does it before the nested call).
        refilled = true;
        W64(kOverhead, R64(m, kOverhead) + k64K);
        const uint64_t blk = NestedExact(root);
        if (blk == 0) {
            return kRefill;
        }
        if (keep_hi > keep_lo && blk < keep_hi && keep_lo < blk + k64K) {
            return kRefill; // the refill takes the call's own run
        }
        Carve(root, blk, k64K);
        return N;
    }

    // The nested exact hit (pool_alloc_region with size 64 KiB, align 8, kind 1): S0 first with 64 KiB alignment (a
    // flexible exact start), else S1 with 1 MiB alignment (a primary exact start). Returns the block's VA, or 0 when the
    // call is not an exact hit (nothing written then).
    uint64_t NestedExact(uint64_t root) {
        const uint64_t S0 = root, S1 = root + 0xa0, S2 = root + 0x140;
        const uint64_t size = k64K;
        if (static_cast<uint64_t>(R32(m, S0 + 0x70)) >= 1) {
            uint64_t n = 0;
            uint64_t p = 0;
            if (Fit(S0, size, k64K, &n, &p)) {
                if (p != 0 || R64(m, n + 0x50) > size) {
                    return 0; // a flexible node hit: not exact
                }
                const uint64_t base = R64(m, n + 0x60);
                Tail(S0, n, size, nullptr);
                Finish(root, S0, n, size, 0, true);
                return base;
            }
        }
        const int64_t reach = int64_t{R32(m, S1 + 0x70)} + R32(m, S1 + 0x74) + R32(m, S2 + 0x70) + R32(m, S2 + 0x74);
        if (reach * static_cast<int64_t>(k64K) < static_cast<int64_t>(size)) {
            return 0;
        }
        if (static_cast<uint64_t>(R32(m, S1 + 0x70)) < 1) {
            return 0;
        }
        uint64_t n = 0;
        uint64_t p = 0;
        if (!Fit(S1, size, k1M, &n, &p) || p != 0 || R64(m, n + 0x50) > size) {
            return 0;
        }
        const uint64_t base = R64(m, n + 0x60);
        Tail(S1, n, size, nullptr);
        Finish(root, S1, n, size, 0, false);
        return base;
    }

    // The carve (0x19722-0x197b3): n = size / 0x70 nodes from base, each pushed at the pool list tail. Seventeen
    // stores per node (C1-C17); the 16-byte zeroing (C7) is one logged write.
    void Carve(uint64_t root, uint64_t base, uint64_t size) {
        const uint64_t S = root + 0x1e0; // the sentinel, as the next link of the tail
        uint64_t tail = R64(m, root + 0x1e8);
        const uint64_t n = size / 0x70;
        for (uint64_t k = 0; k < n; ++k) {
            const uint64_t Nk = base + k * 0x70;
            const uint64_t Ek = Nk + 0x20;
            W64(Nk + 0x00, 0);            // C1
            W64(Nk + 0x08, Nk);           // C2
            W64(Nk + 0x10, Nk);           // C3
            W64(Nk + 0x18, 0);            // C4
            W64(Nk + 0x28, Ek);           // C5 (dead)
            W8(Nk + 0x69, 0);             // C6
            const uint8_t zero16[16] = {};
            m.Write(Nk + 0x50, zero16, 16); // C7
            log.push_back({Nk + 0x50, 16});
            W64(Nk + 0x60, 0);            // C8
            W64(Nk + 0x40, 0);            // C9
            W32(Nk + 0x48, -1);           // C10
            W64(Nk + 0x20, Nk);           // C11
            W64(Nk + 0x30, S);            // C12
            W64(Nk + 0x28, tail);         // C13
            W64(root + 0x1e8, Ek);        // C14
            W64(tail + 0x10, Ek);         // C15
            Inc32(root + 0x200);          // C16
            W64(Nk + 0x38, S);            // C17
            tail = Ek;
        }
    }

    // The exact tail (0x1a974-0x1a9fd) of block B: unlink, region list head, accounting (W1-W15). *next_out gets the
    // value [B+0x30] held before W1 (its node is read at 0x1a980 for the split); nullptr: not needed.
    void Tail(uint64_t S, uint64_t B, uint64_t size, uint64_t* next_out) {
        const uint64_t E = B + 0x20;
        const uint64_t rdx = R64(m, B + 0x30);
        const uint64_t rcx = R64(m, B + 0x28);
        if (next_out != nullptr) {
            *next_out = rdx;
        }
        W64(rcx + 0x10, rdx);                 // W1
        W64(rdx + 0x8, rcx);                  // W2
        W64(B + 0x30, E);                     // W3 (dead)
        Dec32(S + 0x48);                      // W4
        W64(E, B);                            // W5
        W64(E + 0x8, S);                      // W6
        W64(E + 0x10, R64(m, S + 0x10));      // W7: the old region head
        W64(S + 0x10, E);                     // W8
        W64(R64(m, E + 0x10) + 0x8, E);       // W9
        Inc32(S + 0x20);                      // W10
        W64(B + 0x38, S);                     // W11
        const uint64_t bk = R64(m, S + 0x90) + size;
        W64(S + 0x90, bk);                    // W12
        if (bk > R64(m, S + 0x98)) {
            W64(S + 0x98, bk);                // W13
        }
        const uint64_t al = R64(m, S + 0x80) + size;
        W64(S + 0x80, al);                    // W14
        if (al > R64(m, S + 0x88)) {
            W64(S + 0x88, al);                // W15
        }
    }

    // Node fields of the block B (W16-W20), its owner tags, then W22 (0x1aada-0x1abd2). flex: the flexible span's tag
    // table (root+0x20d918, 64 KiB units from the flexible base), else the primary's (root+0x111908, 1 MiB units).
    void Finish(uint64_t root, uint64_t S, uint64_t B, uint64_t size, uint64_t obj, bool flex) {
        W8(B + 0x68, static_cast<uint8_t>(R32(m, S + 0x78)));
        W64(B + 0x58, size);
        W8(B + 0x69, 1);
        W64(B + 0x40, obj);
        W32(B + 0x48, -1);
        const uint64_t base = R64(m, B + 0x60);
        uint64_t addr = 0;
        uint64_t cnt = 0;
        if (flex) {
            const uint64_t fb = R64(m, root + 0x20d908);
            const uint64_t first = (base - fb) >> 16;
            const uint64_t last = (base + size - 1 - fb) >> 16;
            addr = root + 0x20d918 + first;
            cnt = last - first + 1;
        } else {
            const uint64_t first = base >> 20;
            const uint64_t last = (base + size - 1) >> 20;
            addr = root + 0x111908 + first;
            cnt = last - first + 1;
        }
        const uint8_t tag = obj != 0 ? R8(m, obj + 0x1c) : 0;
        std::vector<uint8_t> tags(static_cast<size_t>(cnt), tag);
        m.Write(addr, tags.data(), tags.size());
        log.push_back({addr, tags.size()});
        W32(S + 0x70, static_cast<int32_t>(static_cast<uint32_t>(R32(m, S + 0x70)) - static_cast<uint32_t>(size / k64K)));
    }

    // Returns the block, 0 when the path is not this one (or the guard fails), kRefill when a pop refuses.
    uint64_t Run(uint64_t root, uint64_t size, uint32_t align, int kind, uint64_t obj) {
        const uint64_t pages = size / k64K;
        const uint64_t align64 = (align + 0xffffull) & ~uint64_t{0xffff};
        const uint64_t aarg = (align64 + 0xfffffull) & ~uint64_t{0xfffff};
        const uint64_t S0 = root, S1 = root + 0xa0, S2 = root + 0x140;
        if (kind == 1) {
            uint64_t n = 0;
            uint64_t p = 0;
            if (static_cast<uint64_t>(R32(m, S0 + 0x70)) >= pages && Fit(S0, size, align64, &n, &p)) {
                return 0; // a flexible hit: not this path
            }
        }
        const int64_t reach = (int64_t{R32(m, S1 + 0x70)} + R32(m, S1 + 0x74) + R32(m, S2 + 0x70) + R32(m, S2 + 0x74));
        if (reach * static_cast<int64_t>(k64K) < static_cast<int64_t>(size)) {
            return 0;
        }
        const uint64_t S = kind == 1 ? S1 : S2;
        if (static_cast<uint64_t>(R32(m, S + 0x70)) < pages) {
            return 0;
        }
        uint64_t r = 0;
        uint64_t pad = 0;
        if (!Fit(S, size, aarg, &r, &pad)) {
            return 0;
        }
        const uint64_t split_src = pad != 0 ? R64(m, r + 0x58) - pad : R64(m, r + 0x50);
        if (pad == 0 && !(split_src > size)) {
            return 0; // exact hit
        }
        // The call's own run: its refill must not take it (the walk's run, whole, before any pop).
        keep_lo = R64(m, r + 0x60);
        keep_hi = keep_lo + R64(m, r + 0x58);
        uint64_t B = 0;
        if (pad != 0) {
            const uint64_t M = Pop(root);
            if (M == 0 || M == kRefill) {
                return M;
            }
            // Pad setup (0x1a8b1-0x1a8fa). r's fields are read after the pop.
            const uint64_t rbase = R64(m, r + 0x60);
            const uint64_t rsize = R64(m, r + 0x58);
            const uint8_t rpool = R8(m, r + 0x68);
            W8(M + 0x68, rpool);
            W64(M + 0x58, rsize - pad);
            W64(M + 0x60, rbase + pad);
            W8(M + 0x69, 0);
            W64(M + 0x40, 0);
            W32(M + 0x48, -1);
            W64(M + 0x50, R64(m, r + 0x58) - pad);
            W64(r + 0x58, pad);
            W64(r + 0x50, pad);
            // Insert M after r (0x1a902-0x1a961).
            const uint64_t EM = M + 0x20;
            const uint64_t eq = R64(m, r + 0x30);
            const uint64_t nq = R64(m, eq);
            if (nq != 0) {
                W64(EM, M);
                W64(M + 0x30, nq + 0x20);
                W64(M + 0x28, R64(m, nq + 0x28));
                W64(nq + 0x28, EM);
                W64(R64(m, M + 0x28) + 0x10, EM);
                Inc32(S + 0x48);
            } else {
                W64(EM, M);
                W64(M + 0x30, S + 0x28);
                W64(M + 0x28, R64(m, S + 0x30));
                W64(S + 0x30, EM);
                W64(R64(m, M + 0x28) + 0x10, EM);
                Inc32(S + 0x48);
            }
            W64(M + 0x38, S + 0x28);
            B = M;
        } else {
            B = r;
        }
        uint64_t next_saved = 0;
        Tail(S, B, size, &next_saved);
        if (R64(m, B + 0x50) > size) {
            const uint64_t N2 = Pop(root);
            if (N2 == 0 || N2 == kRefill) {
                return N2;
            }
            const uint64_t bbase = R64(m, B + 0x60);
            const uint64_t bsize = R64(m, B + 0x58);
            const uint8_t bpool = R8(m, B + 0x68);
            W8(N2 + 0x68, bpool);
            W64(N2 + 0x58, bsize - size);
            W64(N2 + 0x60, bbase + size);
            W8(N2 + 0x69, 0);
            W64(N2 + 0x40, 0);
            W32(N2 + 0x48, -1);
            W64(N2 + 0x50, R64(m, B + 0x58) - size);
            W64(B + 0x58, size);
            W64(B + 0x50, size);
            const uint64_t EN2 = N2 + 0x20;
            const uint64_t r12 = R64(m, next_saved);
            if (r12 != 0) {
                W64(N2 + 0x20, N2);
                W64(N2 + 0x30, r12 + 0x20);
                W64(N2 + 0x28, R64(m, r12 + 0x28));
                W64(r12 + 0x28, EN2);
                W64(R64(m, N2 + 0x28) + 0x10, EN2);
                Inc32(S + 0x48);
                W64(N2 + 0x38, S + 0x28);
            } else {
                W64(N2 + 0x20, N2);
                W64(N2 + 0x30, S + 0x28);
                W64(N2 + 0x28, R64(m, S + 0x30));
                W64(S + 0x30, EN2);
                W64(R64(m, N2 + 0x28) + 0x10, EN2);
                Inc32(S + 0x48);
                W64(N2 + 0x38, S + 0x28);
            }
        }
        Finish(root, S, B, size, obj, false);
        return B;
    }
};

// ---------------------------------------------------------------------------------------------------
// One comparison: the production on a copy of the built memory, the reference on another copy.
struct Outcome2 {
    bool refused_by_ref = false; // the reference refused (refill or not this path): the production must refuse
    bool refill = false;         // the reference ran a refill (accepted or not)
    bool accepted = false;       // the reference performed the call
    bool ok = true;
};

Log Sorted(Log l) {
    std::sort(l.begin(), l.end());
    return l;
}

// Compares one scenario. expect_refill: the reference must have run a refill (checked by the caller's expectation).
Outcome2 CompareOne(const Scenario& sc, const Pools& pre, const Prediction& pred, const char* tag,
                    uint64_t overhead_addr = kOverhead) {
    (void)overhead_addr;
    Outcome2 o;
    Mem base;
    Build(base, sc);
    const uint64_t root = kRoot;
    Mem a = base;
    Mem t = base;
    const uint64_t ra = ApplyPrimaryNodeHit(a, root, pre, pred, sc.size, sc.align, sc.kind, sc.obj, kOverhead);
    const auto prod_log = PrimaryLastWriteSet();
    Ref ref(t);
    const uint64_t rt = ref.Run(root, sc.size, sc.align, sc.kind, sc.obj);
    o.refill = ref.refilled;
    if (rt == kRefill || rt == 0) {
        o.refused_by_ref = true;
        const bool same = ra == 0 && SameMem(a, base) && prod_log.empty();
        if (!same) {
            o.ok = false;
            std::printf("  %s: ref refused (%s) but production returned 0x%llx and changed %s\n", tag,
                        rt == kRefill ? "refill or pop" : "not this path", static_cast<unsigned long long>(ra),
                        SameMem(a, base) ? "nothing" : "the memory");
        }
        return o;
    }
    o.accepted = true;
    const bool equal_ret = ra == rt;
    const bool equal_mem = SameMem(a, t);
    const bool equal_log = ref.refilled ? Sorted(prod_log) == Sorted(ref.log) : prod_log == ref.log;
    if (!equal_ret || !equal_mem || !equal_log) {
        o.ok = false;
        std::printf("  %s: ret prod 0x%llx ref 0x%llx, memory %s, writes %s (prod %zu, ref %zu)\n", tag,
                    static_cast<unsigned long long>(ra), static_cast<unsigned long long>(rt),
                    equal_mem ? "equal" : "DIFFERS", equal_log ? "equal" : "DIFFER", prod_log.size(), ref.log.size());
        if (!equal_mem) {
            for (const auto& kv : a.bytes) {
                if (t.Get(kv.first) != kv.second) {
                    std::printf("    first byte differing at 0x%llx: prod %02x ref %02x\n",
                                static_cast<unsigned long long>(kv.first), kv.second, t.Get(kv.first));
                    break;
                }
            }
        }
    }
    return o;
}

// ---------------------------------------------------------------------------------------------------
// Random scenarios.
struct Gen {
    std::mt19937_64 rng;
    explicit Gen(uint64_t seed) : rng(seed) {}
    uint64_t U(uint64_t lo, uint64_t hi) { return std::uniform_int_distribution<uint64_t>(lo, hi)(rng); }

    Scenario Make() {
        Scenario sc;
        sc.kind = U(0, 1) ? 1 : 2;
        // Target set: several runs with random alignment, sizes and backing, so pads and splits both occur.
        for (int s = 0; s < 3; ++s) {
            SetSpec& ss = sc.set[s];
            const int n = s == 0 ? static_cast<int>(U(0, 1)) : static_cast<int>(U(1, 4));
            uint64_t cur = s == 0 ? 0x30000000ull : s == 1 ? 0x300000000ull : 0x400000000ull;
            for (int j = 0; j < n; ++j) {
                RunSpec rs;
                cur += U(0, 40) * k64K + (U(0, 1) ? 0 : U(0, 15) * k1M);
                rs.base = cur;
                rs.size = U(1, 48) * k64K;
                const uint64_t mode = U(0, 3);
                rs.backed = mode == 0 ? rs.size : mode == 1 ? 0 : (U(0, rs.size / k64K) * k64K);
                cur += rs.size;
                ss.runs.push_back(rs);
            }
            // Often a spare exact run for the refill's 64 KiB request, first in the list (1 MiB aligned, backed 0).
            if (s == 1 && U(0, 2) != 0) {
                RunSpec spare;
                spare.base = 0x320000000ull + U(0, 40) * k1M;
                spare.size = k64K;
                spare.backed = 0;
                ss.runs.insert(ss.runs.begin(), spare);
            }
            ss.regions = static_cast<int>(U(0, 3));
            ss.cached = static_cast<int32_t>(U(0, 3));
            ss.alloc = U(0, 1u << 30);
            ss.backed = U(0, 1u << 30);
            ss.alloc_peak = ss.alloc + (U(0, 1) ? 0 : U(0, 1u << 20));
            ss.backed_peak = ss.backed + (U(0, 1) ? 0 : U(0, 1u << 20));
        }
        static const int kCounts[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 40};
        sc.node_count = kCounts[U(0, 9)];
        static const uint32_t kAligns[] = {8, 0x10000, 0x20000, 0x100000, 0x200000, 0x400000};
        sc.align = kAligns[U(0, 5)];
        sc.size = U(1, 40) * k64K;
        sc.obj = U(0, 9) == 0 ? 0 : kObj;
        sc.tag = static_cast<uint8_t>(U(1, 255));
        return sc;
    }
};

// The model prediction for a scenario, through the same Predict the hook uses (no map: the primary hit needs none).
Prediction PredictFor(const Pools& pre, const Scenario& sc, Pools* post) {
    Request req;
    req.size = sc.size;
    req.align = sc.align;
    req.kind = sc.kind;
    req.obj = sc.obj;
    return Predict(pre, req, nullptr, post);
}

// A directed scenario: a primary set with one run that pads (its base is 64 KiB past a 1 MiB boundary) or not, and
// a second, larger run when split is wanted. spare: a 64 KiB exact run first in the list, for the refill's request.
Scenario Directed(bool pad, bool split, int node_count, bool spare) {
    Scenario sc;
    sc.kind = 1;
    sc.align = 0x10000;
    sc.size = 0x10000 * 4;
    sc.obj = kObj;
    sc.tag = 0x5a;
    sc.node_count = node_count;
    SetSpec& s1 = sc.set[1];
    if (spare) {
        RunSpec sp;
        sp.base = 0x310000000ull;
        sp.size = k64K;
        sp.backed = 0;
        s1.runs.push_back(sp);
    }
    // Pad: the run starts 64 KiB past a 1 MiB boundary (pad 0xf0000 to the aligned start). Its size decides the path:
    // 0x130000 leaves the pad's remainder equal to the request (no split), 0x400000 leaves a split. No pad, split:
    // a 1 MiB run whose backed exceeds the request.
    RunSpec rs;
    rs.base = 0x300000000ull + (pad ? 0x10000 : 0);
    if (pad) {
        rs.size = split ? 0x400000 : 0x130000;
        rs.backed = rs.size;
    } else {
        rs.size = 0x100000;
        rs.backed = split ? sc.size + 0x20000 : sc.size; // no split: backed equals the request (exact when unpadded)
    }
    s1.runs.push_back(rs);
    s1.regions = 1;
    s1.alloc = 0x1000000;
    s1.backed = 0x1000000;
    s1.alloc_peak = 0x1000000;
    s1.backed_peak = 0x1000000;
    s1.cached = 0;
    sc.set[2].regions = 0;
    sc.set[2].alloc = 0;
    sc.set[2].backed = 0;
    sc.set[2].alloc_peak = 0;
    sc.set[2].backed_peak = 0;
    return sc;
}

int g_random_ok = 0;
int g_random_refill = 0;
int g_random_ref_refused = 0;
int g_random_paths = 0;

enum class Expect { Accept, Refuse };

// Runs one directed scenario through the model and the comparison. Accept: the reference must perform the call and
// the production must return the block; Refuse: both must refuse with nothing written. refill: the reference must run
// a refill (accept or refuse on it).
void RunDirected(const char* name, const Scenario& sc, Expect want, bool refill) {
    Mem img;
    Pools pre = Build(img, sc);
    Pools post;
    Prediction p = PredictFor(pre, sc, &post);
    std::printf("directed %-40s pred %s (%s)\n", name, PathName(p.path), p.reason);
    if (p.path != Path::PrimaryHitNode) {
        std::printf("  not a primary node hit under the model: FAIL\n");
        ++g_failures;
        return;
    }
    Outcome2 o = CompareOne(sc, pre, p, name);
    CHECK(o.ok);
    CHECK(o.refill == refill);
    if (want == Expect::Accept) {
        CHECK(o.accepted);
        Mem a = img;
        CHECK(ApplyPrimaryNodeHit(a, kRoot, pre, p, sc.size, sc.align, sc.kind, sc.obj, kOverhead) != 0);
    } else {
        CHECK(o.refused_by_ref);
        Mem a = img;
        CHECK(ApplyPrimaryNodeHit(a, kRoot, pre, p, sc.size, sc.align, sc.kind, sc.obj, kOverhead) == 0);
        CHECK(SameMem(a, img));
        CHECK(PrimaryLastWriteSet().empty());
    }
}

} // namespace

int main() {
    // Refills that are accepted (the spare exact run serves the refill's 64 KiB request).
    RunDirected("pad, count 5 (no refill)", Directed(true, false, 5, true), Expect::Accept, false);
    RunDirected("pad, count 9 (no refill)", Directed(true, false, 9, true), Expect::Accept, false);
    RunDirected("split, count 2 (no refill)", Directed(false, true, 2, true), Expect::Accept, false);
    RunDirected("pad+split, count 5 (no refill)", Directed(true, true, 5, true), Expect::Accept, false);
    RunDirected("pad, count 3 (first pop refills)", Directed(true, false, 3, true), Expect::Accept, true);
    RunDirected("split, count 3 (the pop refills)", Directed(false, true, 3, true), Expect::Accept, true);
    RunDirected("pad+split, count 3 (first pop refills)", Directed(true, true, 3, true), Expect::Accept, true);
    RunDirected("pad+split, count 4 (second pop refills)", Directed(true, true, 4, true), Expect::Accept, true);
    // Refills that are refused: no exact run for the refill's request (the pad's own run is not exact at 1 MiB).
    RunDirected("pad, count 3, no spare: refused", Directed(true, false, 3, false), Expect::Refuse, true);
    RunDirected("split, count 3, no spare: refused", Directed(false, true, 3, false), Expect::Refuse, true);
    RunDirected("pad+split, count 4, no spare: refused", Directed(true, true, 4, false), Expect::Refuse, true);
    {
        // Empty node pool: refused whatever the path.
        Scenario sc = Directed(true, false, 0, true);
        Mem img;
        Pools pre = Build(img, sc);
        Pools post;
        Prediction p = PredictFor(pre, sc, &post);
        Mem a = img;
        CHECK(ApplyPrimaryNodeHit(a, kRoot, pre, p, sc.size, sc.align, sc.kind, sc.obj, kOverhead) == 0);
        CHECK(SameMem(a, img));
        std::printf("directed empty node pool: refused, memory unchanged\n");
    }
    {
        // No overhead address: a refilling pop is refused before any write.
        Scenario sc = Directed(true, false, 3, true);
        Mem img;
        Pools pre = Build(img, sc);
        Pools post;
        Prediction p = PredictFor(pre, sc, &post);
        Mem a = img;
        CHECK(ApplyPrimaryNodeHit(a, kRoot, pre, p, sc.size, sc.align, sc.kind, sc.obj, 0) == 0);
        CHECK(SameMem(a, img));
        std::printf("directed refill without overhead address: refused, memory unchanged\n");
    }
    {
        // Owner family unreadable: refused before any write.
        Scenario sc = Directed(true, false, 9, true);
        sc.obj = kObjBad;
        Mem img;
        Pools pre = Build(img, sc);
        Pools post;
        Prediction p = PredictFor(pre, sc, &post);
        Mem a = img;
        CHECK(ApplyPrimaryNodeHit(a, kRoot, pre, p, sc.size, sc.align, sc.kind, sc.obj, kOverhead) == 0);
        CHECK(SameMem(a, img));
        std::printf("directed unreadable owner family: refused, memory unchanged\n");
    }
    {
        // A fabricated exact hit (no pad, backed <= size) passed as a node hit: refused.
        Scenario sc = Directed(false, false, 9, true);
        Mem img;
        Pools pre = Build(img, sc);
        Pools post;
        Prediction p = PredictFor(pre, sc, &post);
        p.path = Path::PrimaryHitNode;
        p.outcome = Outcome::Predicted;
        Mem a = img;
        CHECK(ApplyPrimaryNodeHit(a, kRoot, pre, p, sc.size, sc.align, sc.kind, sc.obj, kOverhead) == 0);
        CHECK(SameMem(a, img));
        std::printf("directed exact hit passed as node hit: refused, memory unchanged\n");
    }
    {
        // Wrong predicted start: refused.
        Scenario sc = Directed(true, false, 9, true);
        Mem img;
        Pools pre = Build(img, sc);
        Pools post;
        Prediction p = PredictFor(pre, sc, &post);
        p.va += 0x10000;
        Mem a = img;
        CHECK(ApplyPrimaryNodeHit(a, kRoot, pre, p, sc.size, sc.align, sc.kind, sc.obj, kOverhead) == 0);
        CHECK(SameMem(a, img));
        std::printf("directed wrong start: refused, memory unchanged\n");
    }

    // Random scenarios: the production against the reference, byte for byte and write for write.
    Gen gen(0x5eed5a11u);
    const int kScenarios = 4000;
    for (int i = 0; i < kScenarios; ++i) {
        Scenario sc = gen.Make();
        Mem img;
        Pools pre = Build(img, sc);
        Pools post;
        Prediction p = PredictFor(pre, sc, &post);
        if (p.path != Path::PrimaryHitNode) {
            continue;
        }
        ++g_random_paths;
        char tag[64];
        std::snprintf(tag, sizeof tag, "random #%d", i);
        Outcome2 o = CompareOne(sc, pre, p, tag);
        CHECK(o.ok);
        if (o.refill) {
            ++g_random_refill;
        }
        if (o.refused_by_ref) {
            ++g_random_ref_refused;
        } else {
            ++g_random_ok;
        }
    }
    std::printf("random: %d primary node predictions: %d performed (%d ran a refill), %d refused by the reference\n",
                g_random_paths, g_random_ok, g_random_refill, g_random_ref_refused);
    CHECK(g_random_ok > 500);
    CHECK(g_random_refill > 0);

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
