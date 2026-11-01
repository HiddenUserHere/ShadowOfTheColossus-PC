// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-native-test. Checks gpu/sotc/pc_pool_native.cpp (ApplyExactHit, and ApplyNodeHit for the flexible
// node path: pad, split, pool pops and the refill refusal, sections "flex node" and "refuse node") against a second,
// literal transcription of pool_alloc_region (0x19e00), pool_take_free_run (0x1a800) and pool_node_alloc (0x19660).
// The exact-hit reference is the first one below:
// the reference below follows the disassembly instruction group by instruction group, including the owner-tag
// jump table (0xf94e20: entry r writes the last r stores of an 8-store block, entry 0 a full block).
// Both run on the same simulated guest memory (a byte map with mapped ranges). Every byte of the result is
// compared, and the write list of ApplyExactHit (LastWriteSet) is compared with the reference's store log.
// Refusal cases must return 0, write nothing and leave the memory unchanged.
// Node pops that take the pool count from 3 to 2 refill (sections "refill"): the reference runs the refill in the game's
// order (bookkeeping add, nested exact hit, carve of 585 nodes); the native runs the nested exact hit before the pop
// stores (the final image is the same), so a refill's write list is compared as a multiset.
//
// Build and run (scratch directory, not the repo). The node pops need pc_pool_native_nodes.cpp, included below:
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_native_test.cpp -o pc_pool_native_test.exe
//   ./pc_pool_native_test.exe   (prints PASS or FAIL; exit code 0 on PASS)
//
// This is a hand transcription, not a machine emulator: a shared misreading of the disassembly would pass both.
// The in-game comparison (the model's PC_POOL hooks) is the check of the real machine.

#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"
#include "../pc_pool_native.cpp"
#include "../pc_pool_native_nodes.cpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace SotcPcPool::Model;
using SotcPcPoolNative::ApplyExactHit;
using SotcPcPoolNative::GuestMem;
using SotcPcPoolNative::LastWriteSet;

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

constexpr uint64_t kR = 0x10000000;           // root
constexpr uint64_t kRootSize = 0x230000;
constexpr uint64_t kNodeExtentModel = 0x80;  // the glue's kNodeExtent
constexpr uint64_t kOv = kR + 0x22f000;       // DAT_02dfca60 for the refill (the root's last page, unused elsewhere)
constexpr uint64_t kFB = 0x200000000ull;      // flexible VA base (root+0x20d908)
constexpr uint64_t kOnion = 0x300000000ull;   // S1 VA base (1 MiB aligned)
constexpr uint64_t kGarlic = 0x400000000ull;  // S2 VA base (1 MiB aligned)
constexpr uint64_t kObj = 0x80000000ull;      // family object
constexpr uint8_t kObjTag = 0x5a;
constexpr uint64_t kNodeArea = 0x60000000ull;
constexpr uint64_t kNoPeak = 1ull << 40;      // peaks far above the values: no peak store
constexpr uint64_t k64K = 0x10000;

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
    void Put(uint64_t a, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            bytes[a + static_cast<uint64_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    void InitMemory() {
        regions.clear();
        regions.push_back({kR, kR + kRootSize});
        regions.push_back({kObj, kObj + 0x100});
        regions.push_back({kNodeArea, kNodeArea + 0x10000000});
        // The flexible VA span: the free runs are mapped here (a refill's block takes a 64 KiB run and is carved).
        regions.push_back({kFB, kFB + 0x10000000ull});
        Put(kOv, 0x1000, 8); // DAT_02dfca60: the bookkeeping qword the refill adds to
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
// Snapshot to guest memory. Nodes come from a bump allocator; base -> node is kept for the checks.
uint64_t g_next_node = kNodeArea;
uint64_t NewNode() {
    const uint64_t n = g_next_node;
    g_next_node += 0x100;
    return n;
}

constexpr uint64_t kSetOff[3] = {0x0, 0xa0, 0x140};

void BuildImage(FakeMem* m, const Pools& p, uint64_t peak_extra, std::map<uint64_t, uint64_t>* node_of_base) {
    m->bytes.clear();
    m->Put(kR + 0x20d908, kFB, 8);
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = kR + kSetOff[k];
        const Set& s = p.set[k];
        // Region list: sentinel S, head S+0x10, tail S+8, elements E = N+0x20, next at E+0x10, prev at E+8.
        std::vector<uint64_t> reg;
        for (int32_t i = 0; i < s.region_count; ++i) {
            reg.push_back(NewNode() + 0x20);
        }
        for (size_t i = 0; i < reg.size(); ++i) {
            const uint64_t E = reg[i];
            m->Put(E, E - 0x20, 8);
            m->Put(E + 8, i == 0 ? S : reg[i - 1], 8);
            m->Put(E + 0x10, i + 1 == reg.size() ? S : reg[i + 1], 8);
        }
        m->Put(S + 0x10, reg.empty() ? S : reg[0], 8);
        m->Put(S + 0x8, reg.empty() ? S : reg.back(), 8);
        // Free-VA list: sentinel F = S+0x28, head S+0x38, tail S+0x30.
        const uint64_t F = S + 0x28;
        std::vector<uint64_t> el;
        for (const Run& r : s.free_va) {
            const uint64_t N = NewNode();
            el.push_back(N + 0x20);
            (*node_of_base)[r.base] = N;
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
        // Header.
        m->Put(S + 0x20, static_cast<uint64_t>(s.region_count), 4);
        m->Put(S + 0x48, static_cast<uint64_t>(s.stored_free_count), 4);
        m->Put(S + 0x70, static_cast<uint64_t>(s.free_va_pages), 4);
        m->Put(S + 0x74, static_cast<uint64_t>(s.cached_phys), 4);
        m->Put(S + 0x78, static_cast<uint64_t>(k), 4);
        m->Put(S + 0x80, s.alloc_bytes, 8);
        m->Put(S + 0x88, s.alloc_bytes + peak_extra, 8);
        m->Put(S + 0x90, s.backed_bytes, 8);
        m->Put(S + 0x98, s.backed_bytes + peak_extra, 8);
    }
    m->Put(kObj + 0x1c, kObjTag, 1);
}

// The snapshot helpers.
Set MakeSet(int id, std::vector<Run> runs, int32_t region_count, int32_t cached, uint64_t alloc,
            uint64_t backed) {
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

// ---------------------------------------------------------------------------------------------------
// Literal reference of the exact-hit path. It logs every store as (address, width); the owner-tag loop is
// logged as one (first address, store count) entry, the same unit the native Write uses.
struct Ref {
    FakeMem* m;
    uint64_t root;
    uint64_t obj;
    std::vector<std::pair<uint64_t, size_t>> log;
    bool not_exact = false;
    bool refill = false;  // a node pop left the pool count at 2: the reference ran the refill (see RefRefill)
    bool refused = false; // the refill was refused (its exact hit is not exact, or it takes the call's own run)
    uint64_t overhead = kOv;      // DAT_02dfca60
    uint64_t keep_lo = 0;         // the call's own run: the refill must not take it
    uint64_t keep_hi = 0;

    uint64_t u64(uint64_t a) const { return m->U64(a); }
    uint32_t u32(uint64_t a) const { return static_cast<uint32_t>(m->U64(a) & 0xffffffffu) ; }
    uint8_t u8(uint64_t a) const { return m->Get(a); }
    void st64(uint64_t a, uint64_t v) { m->Put(a, v, 8); log.push_back({a, 8}); }
    void st32(uint64_t a, uint32_t v) { m->Put(a, v, 4); log.push_back({a, 4}); }
    void st8(uint64_t a, uint8_t v) { m->Put(a, v, 1); log.push_back({a, 1}); }
};

// pool_take_free_run (0x1a800) for the exact path. rbx = size, ecx = align (already rounded by the caller).
// Returns the node (rax), 0 when no run fits, or 0xffffffffffff (sentinel) when the first fit is a pad or a split.
constexpr uint64_t kNotExact = 0xdeadbeef00ull;

uint64_t RefTakeRun(Ref& r, uint64_t S, uint64_t rbx, uint32_t ecx) {
    const uint64_t r13 = rbx >> 16;
    // 0x1a820: cmp dword [S+0x70], r13d ; jb ret0
    if (r.u32(S + 0x70) < static_cast<uint32_t>(r13)) {
        return 0;
    }
    uint64_t rax = r.u64(S + 0x38);
    const uint64_t r8 = S + 0x28;
    uint64_t r12 = 0;
    if (rax != r8) {
        r12 = r.u64(rax);
    }
    const uint64_t rcx = static_cast<uint32_t>(ecx - 1); // dec ecx (zero-extends)
    const uint64_t rdx = ~rcx;
    for (;;) {
        if (r12 == 0) { // 0x1a85d-0x1a860
            return 0;
        }
        const uint64_t size = r.u64(r12 + 0x58);
        if (size >= rbx) { // 0x1a86b-0x1a86e: jb next
            const uint64_t rdi = r.u64(r12 + 0x60);
            uint64_t r14 = (rdi + rcx) & rdx; // 0x1a875-0x1a879
            r14 -= rdi;                        // 0x1a87c
            if (r14 == 0) {                    // 0x1a87f: exact start, node is r12
                break;
            }
            if (r14 + rbx <= size) {           // 0x1a888: jbe pad path (node_alloc)
                return kNotExact;
            }
        }
        // next: 0x1a88a-0x1a89a
        rax = r.u64(r12 + 0x30);
        r12 = r.u64(rax);
    }
    const uint64_t N = r12;
    const uint64_t E = N + 0x20;
    // 0x1a974-0x1a993: unlink
    uint64_t rdx2 = r.u64(N + 0x30);     // next link
    uint64_t rcx2 = r.u64(N + 0x28);     // prev link
    r.st64(rcx2 + 0x10, rdx2);           // W1 0x1a983
    rdx2 = r.u64(N + 0x30);
    r.st64(rdx2 + 0x8, rcx2);            // W2 0x1a98b
    r.st64(N + 0x30, E);                 // W3 0x1a98f
    r.st32(S + 0x48, r.u32(S + 0x48) - 1); // W4 0x1a993 (dec dword)
    // 0x1a997-0x1a9b3: link at the head of the region list
    r.st64(N + 0x20, N);                 // W5
    r.st64(N + 0x28, S);                 // W6
    const uint64_t old = r.u64(S + 0x10);
    r.st64(N + 0x30, old);               // W7
    r.st64(S + 0x10, E);                 // W8
    const uint64_t old2 = r.u64(N + 0x30);
    r.st64(old2 + 0x8, E);               // W9
    r.st32(S + 0x20, r.u32(S + 0x20) + 1); // W10 (inc dword)
    r.st64(N + 0x38, S);                 // W11
    // 0x1a9bb-0x1a9f6: accounting
    uint64_t acc = r.u64(S + 0x90) + rbx;
    r.st64(S + 0x90, acc);               // W12
    if (acc > r.u64(S + 0x98)) {
        r.st64(S + 0x98, acc);           // W13
    }
    acc = r.u64(S + 0x80) + rbx;
    r.st64(S + 0x80, acc);               // W14
    if (acc > r.u64(S + 0x88)) {
        r.st64(S + 0x88, acc);           // W15
    }
    // 0x1a9fd: cmp [N+0x50], rbx ; jbe no-split
    if (r.u64(N + 0x50) > rbx) {
        return kNotExact;                // split path: not this one
    }
    // 0x1aaa0-0x1aaf3 (no-split)
    const uint64_t base = r.u64(N + 0x60);
    const uint32_t edi = r.u32(S + 0x78);
    r.st8(N + 0x68, static_cast<uint8_t>(edi));   // W16
    r.st64(N + 0x58, rbx);                        // W17
    r.st8(N + 0x69, 1);                           // W18
    r.st64(N + 0x40, r.obj);                      // W19
    r.st32(N + 0x48, 0xffffffffu);                // W20
    const uint8_t al = r.obj != 0 ? r.u8(r.obj + 0x1c) : 0; // 0x1aafb-0x1ab02
    // 0x1ab04: tag range
    uint64_t rc = 0;
    uint64_t rd = 0;
    if (edi == 0) { // flexible, 64 KiB units (0x1ab3c-0x1ab63)
        const uint64_t fb = r.u64(r.root + 0x20d908);
        rd = rbx + base - 1;             // lea rdx,[rbx+rcx-1]
        rd -= fb;
        rc = base - fb;
        rd >>= 16;
        rc >>= 16;
        rd += 1;
        rd -= rc;
        rc = r.root + rc + 0x20d918;     // lea rcx,[rsi+rcx+0x20d918]
    } else { // 1 MiB units (0x1ab08-0x1ab32)
        uint64_t rbxt = rbx + base;      // add rbx, rcx
        rc = base >> 20;                 // shr rcx, 0x14
        uint64_t rsi = (0xfffffffffffffull + rbxt) >> 20;
        rd = 1;
        rd -= rc;
        rd += rsi;
        rc = r.root + rc + 0x111908;     // lea rcx,[rdi+rcx+0x111908]
    }
    // 0x1ab6b-0x1abd0: the Duff's device. edx is the 32-bit count.
    const int32_t cnt = static_cast<int32_t>(static_cast<uint32_t>(rd));
    int32_t ebx_ = (((cnt >> 31) & 0x7) + cnt) & ~7;  // sar 31, shr 29 gives 7 for a negative count
    const uint32_t r_idx = static_cast<uint32_t>(cnt - ebx_); // edi
    int32_t esi_ = (((cnt + 7) >> 31) & 0x7);         // sar 31, shr 29
    int32_t edx = (cnt + esi_ + 7) >> 3;              // lea edx,[rdx+rsi+7]; sar edx,3
    if (r_idx > 7) {                                  // 0x1ab85: ja 0x1abd2
        edx = 0;
    } else {
        // Entry r: 0 runs the full 8-store block; r runs the last r stores of it (jump table 0xf94e20).
        const int start = r_idx == 0 ? 0 : static_cast<int>(8 - r_idx);
        uint64_t addr = rc;
        size_t stores = 0;
        for (int i = start; i < 8; ++i) {
            r.m->Put(addr, al, 1);
            ++addr;
            ++stores;
        }
        while (edx > 1) {                              // 0x1abc6-0x1abce
            --edx;
            for (int i = 0; i < 8; ++i) {
                r.m->Put(addr, al, 1);
                ++addr;
                ++stores;
            }
        }
        r.log.push_back({rc, stores});
    }
    // 0x1abd2: sub dword [S+0x70], r13d
    r.st32(S + 0x70, r.u32(S + 0x70) - static_cast<uint32_t>(r13));
    return N;
}

// pool_alloc_region (0x19e00) for va_hint 0 and mode 0: the S0 hit (kind 1), the early reject, the P hit.
uint64_t RefAlloc(Ref& r, uint64_t size, uint32_t align, int kind) {
    const uint32_t ebx = (align + 0xffffu) & 0xffff0000u; // 0x19e2d-0x19e33
    if (kind == 1) {
        const uint64_t n = RefTakeRun(r, r.root, size, ebx); // 0x19e5c-0x19e72
        if (n == kNotExact) {
            r.not_exact = true;
            return 0;
        }
        if (n != 0) {
            return n;
        }
    }
    // 0x19ea1-0x19f04: early reject (mode 0). S1 + S2 free and cached pages, in bytes.
    const uint64_t s1 = (static_cast<uint64_t>(r.u32(r.root + 0xa0 + 0x70)) +
                         r.u32(r.root + 0xa0 + 0x74)) & 0xffffffffu;
    const uint64_t s2 = (static_cast<uint64_t>(r.u32(r.root + 0x140 + 0x70)) +
                         r.u32(r.root + 0x140 + 0x74)) & 0xffffffffu;
    const uint64_t reach = (s1 << 16) + (s2 << 16);
    if (reach < size) {
        return 0;
    }
    const uint32_t e2 = (ebx + 0xfffffu) & 0xfff00000u;   // 0x19f0a-0x19f1b
    const uint64_t S = r.root + (kind == 1 ? 0xa0 : 0x140);
    const uint64_t n = RefTakeRun(r, S, size, e2);
    if (n == kNotExact) {
        r.not_exact = true;
        return 0;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------
// Test driver helpers.
const char* PathStr(Path p) { return PathName(p); }

Request Req(int32_t kind, uint64_t size, uint64_t align) {
    Request q;
    q.kind = kind;
    q.size = size;
    q.align = align;
    return q;
}

bool SameLog(const std::vector<std::pair<uint64_t, size_t>>& a, const std::vector<std::pair<uint64_t, size_t>>& b);

// The same writes in another order (a refill's exact hit is performed before the pop stores by the native).
bool SameMultiset(std::vector<std::pair<uint64_t, size_t>> a, std::vector<std::pair<uint64_t, size_t>> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return SameLog(a, b);
}

bool SameLog(const std::vector<std::pair<uint64_t, size_t>>& a, const std::vector<std::pair<uint64_t, size_t>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// Success case: the model predicts an exact hit, the native applies it, the reference runs the literal path on
// the same image. Also checks the outcome of the simple fields by hand.
void RunExact(const char* name, const Pools& pre, int32_t kind, uint64_t size, uint32_t align, uint64_t obj,
              Path expect_path, uint64_t expect_va, uint64_t peak_extra, bool expect_peak_writes) {
    std::printf("[exact] %s\n", name);
    FakeMem base;
    base.InitMemory();
    std::map<uint64_t, uint64_t> node_of;
    BuildImage(&base, pre, peak_extra, &node_of);

    Pools post;
    const Prediction pred = Predict(pre, Req(kind, size, align), nullptr, &post);
    CHECK(pred.outcome == Outcome::Predicted);
    CHECK(pred.path == expect_path);
    if (pred.path != expect_path) {
        std::printf("  predicted path %s\n", PathStr(pred.path));
        return;
    }
    CHECK(pred.va == expect_va);

    FakeMem nat = base;
    const uint64_t rax = ApplyExactHit(nat, kR, pre, pred, size, align, kind, obj);
    const auto writes = LastWriteSet();
    CHECK(rax != 0);
    CHECK(rax == node_of.at(expect_va));

    FakeMem ref = base;
    Ref rr{&ref, kR, obj, {}, false};
    const uint64_t rrax = RefAlloc(rr, size, align, kind);
    CHECK(!rr.not_exact);
    CHECK(rrax == rax);
    CHECK(SameImage(nat, ref));
    CHECK(SameLog(writes, rr.log));
    if (!SameImage(nat, ref) || !SameLog(writes, rr.log)) {
        std::printf("  writes native=%zu reference=%zu\n", writes.size(), rr.log.size());
    }

    // Hand checks on the result (the node and the set header).
    const uint64_t N = rax;
    const int idx = pred.slot;
    const uint64_t S = kR + kSetOff[idx];
    CHECK(nat.U64(N + 0x60) == expect_va);
    CHECK(nat.U64(N + 0x58) == size);
    CHECK(nat.Get(N + 0x68) == static_cast<uint8_t>(idx));
    CHECK(nat.Get(N + 0x69) == 1);
    CHECK(nat.U64(N + 0x40) == obj);
    CHECK(nat.U64(N + 0x48) == 0xffffffffu); // only the low dword is the -1 store, high dword stays 0
    CHECK(static_cast<uint32_t>(nat.U64(S + 0x70)) ==
          static_cast<uint32_t>(base.U64(S + 0x70)) - static_cast<uint32_t>(size >> 16));
    CHECK(static_cast<uint32_t>(nat.U64(S + 0x20)) == static_cast<uint32_t>(base.U64(S + 0x20)) + 1);
    CHECK(nat.U64(S + 0x80) == base.U64(S + 0x80) + size);
    CHECK(nat.U64(S + 0x90) == base.U64(S + 0x90) + size);
    CHECK(nat.U64(S + 0x10) == N + 0x20);
    const uint64_t old_head = base.U64(S + 0x10);
    CHECK(nat.U64(old_head + 0x8) == N + 0x20);
    CHECK(nat.U64(N + 0x38) == S);
    // Owner tag: the units of the range hold obj's tag byte.
    if (idx == 0) {
        const uint64_t first = (expect_va - kFB) >> 16;
        const uint64_t last = (expect_va + size - 1 - kFB) >> 16;
        for (uint64_t u = first; u <= last; ++u) {
            CHECK(nat.Get(kR + 0x20d918 + u) == (obj ? kObjTag : 0));
        }
    } else {
        const uint64_t first = expect_va >> 20;
        const uint64_t last = (expect_va + size - 1) >> 20;
        for (uint64_t u = first; u <= last; ++u) {
            CHECK(nat.Get(kR + 0x111908 + u) == (obj ? kObjTag : 0));
        }
    }
    // Peak stores present or not, as the model's comparison expects.
    bool wrote_peak = false;
    for (const auto& w : writes) {
        if (w.first == S + 0x88 || w.first == S + 0x98) {
            wrote_peak = true;
        }
    }
    CHECK(wrote_peak == expect_peak_writes);
    std::printf("  node %llx, writes %zu\n", static_cast<unsigned long long>(rax), writes.size());
}

// Refusal: the native returns 0, writes nothing and leaves memory as it was.
void RunRefuse(const char* name, const FakeMem& base, const Pools& pre, const Prediction& pred, uint64_t size,
               uint32_t align, int kind, uint64_t obj) {
    FakeMem mem = base;
    const uint64_t rax = ApplyExactHit(mem, kR, pre, pred, size, align, kind, obj);
    const bool ok = rax == 0 && LastWriteSet().empty() && SameImage(mem, base);
    CHECK(ok);
    std::printf("[refuse] %s: %s\n", name, ok ? "ok" : "BAD");
}

// Predicted call for a snapshot (used by the refusals that alter one field).
Prediction PredictOf(const Pools& pre, int32_t kind, uint64_t size, uint64_t align) {
    Pools post;
    return Predict(pre, Req(kind, size, align), nullptr, &post);
}

// ---------------------------------------------------------------------------------------------------
// Flexible node path (ApplyNodeHit). The reference transcribes the pad and split branches of pool_take_free_run
// (0x1a800, S0) and pool_node_alloc (0x19660) on the direct memory, in the disassembly's order, with the same
// store log units as the exact reference (one entry per store, the owner tags as one entry).

// The node pool: sentinel root+0x1e0 (next link at root+0x1f0 = head, prev link at root+0x1e8 = tail), count at
// root+0x200. Elements E = N+0x20 with E+0 = N, E+8 = prev, E+0x10 = next.
void BuildNodePool(FakeMem* m, int count) {
    const uint64_t K = kR + 0x1e0;
    std::vector<uint64_t> el;
    for (int i = 0; i < count; ++i) {
        el.push_back(NewNode() + 0x20);
    }
    for (size_t i = 0; i < el.size(); ++i) {
        const uint64_t E = el[i];
        m->Put(E, E - 0x20, 8);
        m->Put(E + 8, i == 0 ? K : el[i - 1], 8);
        m->Put(E + 0x10, i + 1 == el.size() ? K : el[i + 1], 8);
        m->Put(E - 0x20 + 0x38, 0, 8);
    }
    m->Put(kR + 0x1f0, el.empty() ? K : el[0], 8);
    m->Put(kR + 0x1e8, el.empty() ? K : el.back(), 8);
    m->Put(kR + 0x200, static_cast<uint64_t>(count), 4);
}

// pool_node_alloc (0x19660). The stores in order: N+0x38, N+0x20, prev+0x10, next+8, N+0x30, N+0x28, the count.
// A pop that leaves the count at 2 sets refill (0x196c9: the game calls pool_alloc_region 0x19e00 next).
// The carve (0x19722-0x197b3) of the refill's block: n = size / 0x70 nodes from base, each pushed at the pool list tail.
// Seventeen stores per node (C1-C17); the 16-byte zeroing (C7) is one logged write.
void RefCarve(Ref& r, uint64_t base, uint64_t size) {
    const uint64_t root = r.root;
    const uint64_t S = root + 0x1e0; // the node pool sentinel (the next link of the last node)
    uint64_t tail = r.u64(root + 0x1e8);
    const uint64_t n = size / 0x70;
    for (uint64_t k = 0; k < n; ++k) {
        const uint64_t Nk = base + k * 0x70;
        const uint64_t Ek = Nk + 0x20;
        r.st64(Nk + 0x00, 0);            // C1
        r.st64(Nk + 0x08, Nk);           // C2
        r.st64(Nk + 0x10, Nk);           // C3
        r.st64(Nk + 0x18, 0);            // C4
        r.st64(Nk + 0x28, Ek);           // C5 (dead)
        r.st8(Nk + 0x69, 0);             // C6
        r.m->Put(Nk + 0x50, 0, 8);       // C7: 16 zero bytes, one logged write
        r.m->Put(Nk + 0x58, 0, 8);
        r.log.push_back({Nk + 0x50, 16});
        r.st64(Nk + 0x60, 0);            // C8
        r.st64(Nk + 0x40, 0);            // C9
        r.st32(Nk + 0x48, 0xffffffffu);  // C10
        r.st64(Nk + 0x20, Nk);           // C11
        r.st64(Nk + 0x30, S);            // C12
        r.st64(Nk + 0x28, tail);         // C13
        r.st64(root + 0x1e8, Ek);        // C14
        r.st64(tail + 0x10, Ek);         // C15
        r.st32(root + 0x200, r.u32(root + 0x200) + 1); // C16
        r.st64(Nk + 0x38, S);            // C17
        tail = Ek;
    }
}

// The refill of pool_node_alloc (0x196c9-0x19722), in the game's order: R0 the bookkeeping add, R1 the nested
// pool_alloc_region (64 KiB, align 8, kind 1), which must be an exact hit, then the carve. The nested block must not
// take the call's own run (r.keep_lo/keep_hi). Returns false when refused (nothing of the refill is carved then).
bool RefRefill(Ref& r) {
    r.st64(r.overhead, r.u64(r.overhead) + 0x10000); // R0
    // R1: pool_node_alloc calls pool_alloc_region with obj = 0 (the block's owner family and tag are 0).
    const uint64_t call_obj = r.obj;
    r.obj = 0;
    const uint64_t n = RefAlloc(r, 0x10000, 8, 1);
    r.obj = call_obj;
    if (r.not_exact || n == 0) {
        return false;
    }
    const uint64_t base = r.u64(n + 0x60);
    if (r.keep_hi > r.keep_lo && base < r.keep_hi && r.keep_lo < base + 0x10000) {
        return false;
    }
    RefCarve(r, base, 0x10000);
    return true;
}

uint64_t RefNodeAlloc(Ref& r) {
    const uint64_t root = r.root;
    const uint64_t E = r.u64(root + 0x1f0);
    if (E == root + 0x1e0) {
        return 0; // empty pool: the game would dereference the sentinel; the reference stops here
    }
    const uint64_t N = r.u64(E);
    const uint64_t rdx0 = r.u64(N + 0x30);
    r.st64(N + 0x38, 0);
    r.st64(N + 0x20, 0);
    const uint64_t E1 = N + 0x20;
    const uint64_t rsi = r.u64(N + 0x28);
    r.st64(rsi + 0x10, rdx0);
    const uint64_t rdx1 = r.u64(N + 0x30);
    r.st64(rdx1 + 8, rsi);
    r.st64(N + 0x30, E1);
    r.st64(N + 0x28, E1);
    const int32_t cnt = static_cast<int32_t>(r.u32(root + 0x200)) - 1;
    r.st32(root + 0x200, static_cast<uint32_t>(cnt));
    if (cnt == 2) {
        r.refill = true;
        if (!RefRefill(r)) {
            r.refused = true;
        }
    }
    return N;
}

// The sentinel F of the S0 free-VA list (S+0x28).
uint64_t F_of(uint64_t S) { return S + 0x28; }

// The flexible node path of 0x1a800 with S0 (rbx = size, ecx = the 64 KiB-rounded alignment). Returns the block node,
// or 0 with r.not_exact (an exact start: not this path), or 0 with r.refused (a pop's refill is refused), or 0 (no fit).
uint64_t RefFlexNode(Ref& r, uint64_t rbx, uint32_t ecx_arg) {
    const uint64_t S = r.root;
    const uint64_t r13 = rbx >> 16;
    if (r.u32(S + 0x70) < static_cast<uint32_t>(r13)) { // 0x1a820
        return 0;
    }
    uint64_t rax = r.u64(S + 0x38);
    const uint64_t r8 = S + 0x28;
    uint64_t r12 = 0;
    if (rax != r8) {
        r12 = r.u64(rax);
    }
    const uint64_t rcx = static_cast<uint32_t>(ecx_arg - 1);
    const uint64_t rdx = ~rcx;
    uint64_t r14 = 0;
    for (;;) { // 0x1a858-0x1a89a: the first fit, pad (r14) as the game computes it
        if (r12 == 0) {
            return 0;
        }
        const uint64_t size = r.u64(r12 + 0x58);
        if (size >= rbx) {
            const uint64_t rdi = r.u64(r12 + 0x60);
            r14 = ((rdi + rcx) & rdx) - rdi;
            if (r14 == 0) {
                break; // exact start (0x1a8a1)
            }
            if (r14 + rbx <= size) {
                break; // pad path (0x1a8a9)
            }
        }
        rax = r.u64(r12 + 0x30);
        r12 = r.u64(rax);
    }
    const uint64_t N0 = r12;
    // The call's own run: a refill of one of its pops must not take it.
    r.keep_lo = r.u64(N0 + 0x60);
    r.keep_hi = r.keep_lo + r.u64(N0 + 0x58);
    if (r14 == 0 && r.u64(N0 + 0x50) <= rbx) { // 0x1a9fd: no split: the exact path
        r.not_exact = true;
        return 0;
    }
    uint64_t B = N0;
    if (r14 != 0) {
        // 0x1a8a9-0x1a8fa: A is the pad node; A takes [base+pad, end), N keeps [base, base+pad).
        const uint64_t A = RefNodeAlloc(r);
        if (r.refused || A == 0) {
            return 0;
        }
        const uint8_t dl = r.u8(N0 + 0x68);
        const uint64_t rsi = r.u64(N0 + 0x60) + r14;
        const uint64_t rcx1 = r.u64(N0 + 0x58) - r14;
        r.st8(A + 0x68, dl);
        r.st64(A + 0x58, rcx1);
        r.st64(A + 0x60, rsi);
        r.st8(A + 0x69, 0);
        r.st64(A + 0x40, 0);
        r.st32(A + 0x48, 0xffffffffu);
        r.st64(A + 0x50, r.u64(N0 + 0x58) - r14);
        r.st64(N0 + 0x58, r14);
        r.st64(N0 + 0x50, r14);
        // 0x1a902-0x1a961: insert A after N.
        const uint64_t nx = r.u64(N0 + 0x30);
        const uint64_t nxn = r.u64(nx);
        const uint64_t EA = A + 0x20;
        r.st64(EA, A);
        if (nxn == 0) {
            r.st64(A + 0x30, F_of(S));
            const uint64_t ot = r.u64(S + 0x30);
            r.st64(A + 0x28, ot);
            r.st64(S + 0x30, EA);
            const uint64_t pv = r.u64(A + 0x28);
            r.st64(pv + 0x10, EA);
            r.st32(S + 0x48, r.u32(S + 0x48) + 1);
        } else {
            r.st64(A + 0x30, nxn + 0x20);
            const uint64_t pe = r.u64(nxn + 0x28);
            r.st64(A + 0x28, pe);
            r.st64(nxn + 0x28, EA);
            const uint64_t pv = r.u64(A + 0x28);
            r.st64(pv + 0x10, EA);
            r.st32(S + 0x48, r.u32(S + 0x48) + 1);
        }
        r.st64(A + 0x38, F_of(S));
        B = A;
    }
    // 0x1a974-0x1a9f6: unlink B, link it into the region list, the accounting.
    const uint64_t bE = B + 0x20;
    uint64_t rdx2 = r.u64(B + 0x30);
    const uint64_t rcx3 = r.u64(B + 0x28);
    const uint64_t next_node = r.u64(rdx2);
    r.st64(rcx3 + 0x10, rdx2);
    rdx2 = r.u64(B + 0x30);
    r.st64(rdx2 + 8, rcx3);
    r.st64(B + 0x30, bE);
    r.st32(S + 0x48, r.u32(S + 0x48) - 1);
    r.st64(bE, B);
    r.st64(bE + 8, S);
    const uint64_t old_head = r.u64(S + 0x10);
    r.st64(bE + 0x10, old_head);
    r.st64(S + 0x10, bE);
    const uint64_t old_head2 = r.u64(bE + 0x10);
    r.st64(old_head2 + 8, bE);
    r.st32(S + 0x20, r.u32(S + 0x20) + 1);
    r.st64(B + 0x38, S);
    uint64_t acc = r.u64(S + 0x90) + rbx;
    r.st64(S + 0x90, acc);
    if (acc > r.u64(S + 0x98)) {
        r.st64(S + 0x98, acc);
    }
    acc = r.u64(S + 0x80) + rbx;
    r.st64(S + 0x80, acc);
    if (acc > r.u64(S + 0x88)) {
        r.st64(S + 0x88, acc);
    }
    // 0x1a9fd-0x1aa69: the split. R takes [base+size, end) of B; B becomes size.
    if (r.u64(B + 0x50) > rbx) {
        const uint64_t R = RefNodeAlloc(r);
        if (r.refused || R == 0) {
            return 0;
        }
        const uint64_t rdi = r.u64(B + 0x60) + rbx;
        const uint64_t rcx4 = r.u64(B + 0x58) - rbx;
        const uint8_t dl2 = r.u8(B + 0x68);
        r.st8(R + 0x68, dl2);
        r.st64(R + 0x58, rcx4);
        r.st64(R + 0x60, rdi);
        r.st8(R + 0x69, 0);
        r.st64(R + 0x40, 0);
        r.st32(R + 0x48, 0xffffffffu);
        r.st64(R + 0x50, r.u64(B + 0x58) - rbx);
        r.st64(B + 0x58, rbx);
        r.st64(B + 0x50, rbx);
        const uint64_t RE = R + 0x20;
        if (next_node != 0) {
            r.st64(RE, R);                                      // 0x1aa74
            r.st64(R + 0x30, next_node + 0x20);                 // 0x1aa78
            const uint64_t pn = r.u64(next_node + 0x28);
            r.st64(R + 0x28, pn);                               // 0x1aa81
            r.st64(next_node + 0x28, RE);                       // 0x1aa85
            const uint64_t pv = r.u64(R + 0x28);
            r.st64(pv + 0x10, RE);                              // 0x1aa8e
            r.st32(S + 0x48, r.u32(S + 0x48) + 1);              // 0x1aa92
        } else {
            r.st64(RE, R);                                      // 0x1aaae
            r.st64(R + 0x30, F_of(S));                          // 0x1aab6
            const uint64_t ot = r.u64(S + 0x30);
            r.st64(R + 0x28, ot);                               // 0x1aabe
            r.st64(S + 0x30, RE);                               // 0x1aac2
            const uint64_t pv = r.u64(R + 0x28);
            r.st64(pv + 0x10, RE);                              // 0x1aaca
            r.st32(S + 0x48, r.u32(S + 0x48) + 1);              // 0x1aace
        }
        r.st64(R + 0x38, F_of(S));                              // 0x1aa9a / 0x1aad2
    }
    // 0x1aada-0x1abd2: B's fields, the owner tags, the free pages.
    const uint32_t edi = r.u32(S + 0x78);
    r.st8(B + 0x68, static_cast<uint8_t>(edi));
    r.st64(B + 0x58, rbx);
    r.st8(B + 0x69, 1);
    r.st64(B + 0x40, r.obj);
    r.st32(B + 0x48, 0xffffffffu);
    const uint8_t al = r.obj != 0 ? r.u8(r.obj + 0x1c) : 0;
    const uint64_t fb = r.u64(r.root + 0x20d908);
    const uint64_t base = r.u64(B + 0x60);
    const uint64_t first = (base - fb) >> 16;
    const uint64_t last = (base + rbx - 1 - fb) >> 16;
    const uint64_t cnt = last - first + 1;
    for (uint64_t i = 0; i < cnt; ++i) {
        r.m->Put(r.root + 0x20d918 + first + i, al, 1);
    }
    r.log.push_back({r.root + 0x20d918 + first, static_cast<size_t>(cnt)});
    r.st32(S + 0x70, r.u32(S + 0x70) - static_cast<uint32_t>(r13));
    return B;
}

// The S0 free-VA list in list order, from the image (base, size, backed).
std::vector<Run> WalkFreeImage(const FakeMem& m, uint64_t S) {
    std::vector<Run> out;
    const uint64_t F = S + 0x28;
    uint64_t e = m.U64(S + 0x38);
    for (int guard = 0; e != F && guard < 1000; ++guard) {
        const uint64_t N = m.U64(e);
        out.push_back(Run{m.U64(N + 0x60), m.U64(N + 0x58), m.U64(N + 0x50), N});
        e = m.U64(e + 0x10);
    }
    return out;
}

bool SameFree(const std::vector<Run>& got, const std::vector<Run>& want) {
    if (got.size() != want.size()) {
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].base != want[i].base || got[i].size != want[i].size || got[i].backed != want[i].backed) {
            return false;
        }
    }
    return true;
}

// Runs the node path natively and in the reference on one image. expect_refuse: the node pool count makes the game
// refill, so the native must refuse with nothing written (and the reference must report the refill).
// pops: the node pool pops of a successful call (1 for a pad or a split, 2 for both).
void RunNode(const char* name, const Pools& pre, uint64_t size, uint32_t align, int node_count, int pops,
             uint64_t expect_va, const std::vector<Run>& free_expect, bool expect_refuse, uint64_t obj,
             uint64_t peak_extra = kNoPeak, bool expect_refill = false, bool check_free = true) {
    std::printf("[node] %s\n", name);
    FakeMem base;
    base.InitMemory();
    std::map<uint64_t, uint64_t> node_of;
    BuildImage(&base, pre, peak_extra, &node_of);
    BuildNodePool(&base, node_count);

    Pools post;
    const Prediction pred = Predict(pre, Req(1, size, align), nullptr, &post);
    CHECK(pred.outcome == Outcome::Predicted);
    CHECK(pred.path == Path::FlexHitNode);
    CHECK(pred.va == expect_va);
    if (pred.path != Path::FlexHitNode || pred.va != expect_va) {
        std::printf("  prediction differs: path %s\n", PathStr(pred.path));
        return;
    }

    // The model's composed post state of the call, the refill's exact hit and carve included, when the model covers the
    // call (PredictNodeCall); the outer prediction is the same as Predict's.
    SotcPcPoolNodes::PopPrediction pinfo;
    Pools post_model;
    const Prediction composed = SotcPcPoolNodes::PredictNodeCall(pre, Req(1, size, align), nullptr, &post_model,
                                                                 node_count, &pinfo);
    if (pinfo.ok) {
        CHECK(composed.path == pred.path && composed.va == pred.va);
        CHECK(pinfo.refill == expect_refill);
        post = post_model;
    } else {
        std::printf("  model: the call is not modelled (%s); post checks skipped\n", pinfo.reason);
    }

    FakeMem nat = base;
    const uint64_t rax = ApplyNodeHit(nat, kR, pre, pred, size, align, 1, obj, kOv);
    const auto writes = LastWriteSet();
    // The in-game guard: a native write must land in the set headers, the node pool header, the owner-tag tables, a
    // node or a block. Root data beyond those (root+0x208 .. root+0x20d918) is a bug, whatever the game does.
    for (const auto& w : writes) {
        const bool in_root = w.first >= kR && w.first < kR + kRootSize;
        const bool hdr = w.first + w.second <= kR + 0x208;
        const bool tags = w.first >= kR + 0x20d918 && w.first < kR + 0x22f000;
        const bool ov = w.first == kOv;
        if (in_root && !hdr && !tags && !ov) {
            CHECK(false);
            std::printf("  root write outside the allow-list: 0x%llx (+%zu)\n",
                        static_cast<unsigned long long>(w.first), w.second);
        }
    }

    FakeMem ref = base;
    Ref rr{&ref, kR, obj, {}, false, false};
    const uint64_t rrax = RefFlexNode(rr, size, (align + 0xffffu) & 0xffff0000u);
    CHECK(rr.refill == expect_refill);
    CHECK(rr.refused == expect_refuse);
    if (expect_refuse) {
        CHECK(rax == 0);
        CHECK(writes.empty());
        CHECK(SameImage(nat, base));
        std::printf("  refused, the refill is refused: %s\n", rax == 0 && writes.empty() ? "ok" : "BAD");
        return;
    }
    CHECK(!rr.refused);
    CHECK(!rr.not_exact);
    CHECK(rrax != 0);
    CHECK(rax != 0);
    CHECK(rax == rrax);
    CHECK(SameImage(nat, ref));
    CHECK(rr.refill ? SameMultiset(writes, rr.log) : SameLog(writes, rr.log));
    if (!SameImage(nat, ref) || !(rr.refill ? SameMultiset(writes, rr.log) : SameLog(writes, rr.log))) {
        std::printf("  writes native=%zu reference=%zu\n", writes.size(), rr.log.size());
    }

    // Hand checks: the block, the set, the node pool, the free list, the owner tags.
    const uint64_t S = kR;
    CHECK(rax != 0 && nat.U64(rax + 0x60) == expect_va);
    CHECK(nat.U64(rax + 0x58) == size);
    CHECK(nat.Get(rax + 0x68) == 0);
    CHECK(nat.Get(rax + 0x69) == 1);
    CHECK(nat.U64(rax + 0x40) == obj);
    CHECK(static_cast<uint32_t>(nat.U64(rax + 0x48)) == 0xffffffffu);
    // The region list head is the block's node unless a refill ran after the block's tail (a split's pop): then the
    // nested exact hit's node is the head (the image comparison above covers it).
    if (!expect_refill) {
        CHECK(nat.U64(S + 0x10) == rax + 0x20);
    }
    CHECK(nat.U64(rax + 0x38) == S);
    // The refill's nested exact hit takes one page of S0 too (a 64 KiB request).
    CHECK(static_cast<uint32_t>(nat.U64(S + 0x70)) ==
          static_cast<uint32_t>(base.U64(S + 0x70)) - static_cast<uint32_t>(size >> 16) - (expect_refill ? 1u : 0u));
    CHECK(static_cast<uint32_t>(nat.U64(kR + 0x200)) ==
          static_cast<uint32_t>(node_count - pops + (expect_refill ? 585 : 0)));
    CHECK(nat.U64(kOv) == (expect_refill ? base.U64(kOv) + 0x10000 : base.U64(kOv)));
    if (check_free) {
        CHECK(SameFree(WalkFreeImage(nat, S), free_expect));
    }
    if (check_free && !SameFree(WalkFreeImage(nat, S), free_expect)) {
        std::printf("  free list differs from the expected runs; native list:");
        for (const Run& got : WalkFreeImage(nat, S)) {
            std::printf(" [%llx +%llx backed %llx]", static_cast<unsigned long long>(got.base),
                        static_cast<unsigned long long>(got.size), static_cast<unsigned long long>(got.backed));
        }
        std::printf("\n");
    }
    // The model's post state (what the controller compares with) must agree with the native image, when the model
    // covers the call (pinfo.ok; a second pop's refill is not modelled).
    if (pinfo.ok) {
        CHECK(SameFree(WalkFreeImage(nat, S), post.set[0].free_va));
        CHECK(static_cast<uint32_t>(nat.U64(S + 0x70)) == static_cast<uint32_t>(post.set[0].free_va_pages));
        CHECK(static_cast<int32_t>(nat.U64(S + 0x20)) == post.set[0].region_count);
        CHECK(static_cast<int32_t>(nat.U64(S + 0x48)) == post.set[0].stored_free_count);
        CHECK(nat.U64(S + 0x80) == post.set[0].alloc_bytes);
        CHECK(nat.U64(S + 0x90) == post.set[0].backed_bytes);
    }
    const uint8_t want_tag = obj != 0 ? kObjTag : 0;
    const uint64_t first = (expect_va - kFB) >> 16;
    const uint64_t last = (expect_va + size - 1 - kFB) >> 16;
    for (uint64_t u = first; u <= last; ++u) {
        CHECK(nat.Get(kR + 0x20d918 + u) == want_tag);
    }
    std::printf("  node %llx, writes %zu\n", static_cast<unsigned long long>(rax), writes.size());
}

// Refusal of the node path: the native returns 0, writes nothing, leaves memory as it was.
void RunRefuseNode(const char* name, const FakeMem& base, const Pools& pre, const Prediction& pred, uint64_t size,
                   uint32_t align, int kind, uint64_t obj, uint64_t overhead = kOv) {
    FakeMem mem = base;
    const uint64_t rax = ApplyNodeHit(mem, kR, pre, pred, size, align, kind, obj, overhead);
    const bool ok = rax == 0 && LastWriteSet().empty() && SameImage(mem, base);
    CHECK(ok);
    std::printf("[refuse node] %s: %s\n", name, ok ? "ok" : "BAD");
}

void NodeCases() {
    // N1: pad only. Run N = [kFB+0x10000, 0x40000); aligned start kFB+0x20000, pad 0x10000; A = 0x30000 = request.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("flex node, pad only, obj set", pre, 0x30000, 0x20000, 4, 1, kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, kObj);
        RunNode("flex node, pad only, obj NULL", pre, 0x30000, 0x20000, 4, 1, kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, 0);
    }
    // N2: split only. First fit at the exact start, backed 0x40000 above the request 0x30000.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x40000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("flex node, split only, obj set", pre, 0x30000, 0x10000, 4, 1, kFB + 0x40000,
                {Run{kFB + 0x70000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, kObj);
    }
    // N3: pad and split. A = [kFB+0x20000, 0x70000 bytes), split at 0x30000: R = [kFB+0x50000, 0x40000).
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x80000, 0x80000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("flex node, pad and split, obj set", pre, 0x30000, 0x20000, 5, 2, kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x50000, 0x40000, 0x40000},
                 Run{kFB + 0x100000, 0x100000, 0}},
                false, kObj);
        RunNode("flex node, pad and split, obj NULL", pre, 0x30000, 0x20000, 5, 2, kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x50000, 0x40000, 0x40000},
                 Run{kFB + 0x100000, 0x100000, 0}},
                false, 0);
    }
    // N3b: the free run is the last element of the list: the pad node is inserted at the tail (sentinel next).
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("flex node, pad only, run is the last element (tail insert)", pre, 0x30000, 0x20000, 4, 1,
                kFB + 0x20000, {Run{kFB + 0x10000, 0x10000, 0x10000}}, false, kObj);
    }
    // N2b: split only, the block is the last element: the remainder is inserted at the tail.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x40000, 0x40000, 0x40000}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("flex node, split only, block is the last element (tail insert)", pre, 0x30000, 0x10000, 4, 1,
                kFB + 0x40000, {Run{kFB + 0x70000, 0x10000, 0x10000}}, false, kObj);
    }
    // N3b: pad and split, the run is the last element: both inserts at the tail.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x80000, 0x80000}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("flex node, pad and split, run is the last element (tail inserts)", pre, 0x30000, 0x20000, 5, 2,
                kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x50000, 0x40000, 0x40000}}, false, kObj);
    }
    // N3c: the new totals equal the peaks: the game's compare is strict, so no peak store.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("flex node, pad only, peaks equal to the new totals", pre, 0x30000, 0x20000, 4, 1, kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, kObj, 0x30000);
    }
    // N4: the pool refill threshold (a pop that leaves the count at 2 refills: 64 KiB through the nested exact hit, then
    // the carve of 585 nodes). The refill's request is an exact hit only when S0 has a 64 KiB run that starts on a 64 KiB
    // boundary with backed <= 64 KiB and comes first in the list (the "spare"); without one the refill is refused.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x800000, 0x10000, 0}, Run{kFB + 0x10000, 0x40000, 0x40000},
                             Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("pad only, count 3: the pop refills, the refill takes the spare", pre, 0x30000, 0x20000, 3, 1,
                kFB + 0x20000, {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, kObj,
                kNoPeak, true);
        RunNode("pad only, count 3 with the spare, object NULL", pre, 0x30000, 0x20000, 3, 1, kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, 0, kNoPeak, true);
    }
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("pad only, count 3, no spare (the refill is refused)", pre, 0x30000, 0x20000, 3, 1, kFB + 0x20000, {},
                true, kObj, kNoPeak, true);
        RunNode("pad only, count 2 is not reached: accepted", pre, 0x30000, 0x20000, 2, 1, kFB + 0x20000,
                {Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, kObj);
    }
    // N4 pad and split: the first pop refills at count 3 (the second pop then runs at 587), the second pop refills at
    // count 4 (after the tail's writes; the model does not cover that case, the native and the reference do).
    {
        const std::vector<Run> free_pad_split = {Run{kFB + 0x10000, 0x10000, 0x10000},
                                                 Run{kFB + 0x50000, 0x40000, 0x40000},
                                                 Run{kFB + 0x100000, 0x100000, 0}};
        Set s0 = MakeSet(0, {Run{kFB + 0x800000, 0x10000, 0}, Run{kFB + 0x10000, 0x80000, 0x80000},
                             Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("pad and split, count 3 (first pop refills, spare)", pre, 0x30000, 0x20000, 3, 2, kFB + 0x20000,
                free_pad_split, false, kObj, kNoPeak, true);
        RunNode("pad and split, count 4 (second pop refills, spare)", pre, 0x30000, 0x20000, 4, 2, kFB + 0x20000,
                free_pad_split, false, kObj, kNoPeak, true);
        RunNode("pad and split, count 5 (no refill, spare unused)", pre, 0x30000, 0x20000, 5, 2, kFB + 0x20000,
                {Run{kFB + 0x800000, 0x10000, 0}, Run{kFB + 0x10000, 0x10000, 0x10000}, Run{kFB + 0x50000, 0x40000, 0x40000},
                 Run{kFB + 0x100000, 0x100000, 0}},
                false, kObj);
    }
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x80000, 0x80000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("pad and split, count 3, no spare (refused)", pre, 0x30000, 0x20000, 3, 2, kFB + 0x20000, {}, true,
                kObj, kNoPeak, true);
    }
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x800000, 0x10000, 0}, Run{kFB + 0x40000, 0x40000, 0x40000},
                             Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("split only, count 3 (the pop refills, spare)", pre, 0x30000, 0x10000, 3, 1, kFB + 0x40000,
                {Run{kFB + 0x70000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0}}, false, kObj, kNoPeak, true);
    }
    {
        // The refill's exact start on the last run (backed 0): the refill takes it, its node keeps 64 KiB and the
        // rest of the run leaves the free list (the exact path drops the run's tail).
        Set s0 = MakeSet(0, {Run{kFB + 0x40000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        // The refill takes the run after the block, which the remainder is linked before (0x1a980 reads it before the
        // pop): the model does not predict it (pinfo.ok false), the native and the reference must still agree.
        RunNode("split only, count 3, the refill takes the run after the block (unmodelled)", pre, 0x30000, 0x10000, 3,
                1, kFB + 0x40000, {}, false, kObj, kNoPeak, true, false);
    }
    {
        // The last run is not an exact start for the refill (backed above 64 KiB): refused, nothing written.
        Set s0 = MakeSet(0, {Run{kFB + 0x40000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0x100000}}, 0, 0, 0,
                         0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunNode("split only, count 3, no exact run for the refill (refused)", pre, 0x30000, 0x10000, 3, 1,
                kFB + 0x40000, {}, true, kObj, kNoPeak, true);
        RunNode("split only, count 2 (the last run is not exact: unchanged)", pre, 0x30000, 0x10000, 2, 1,
                kFB + 0x40000,
                {Run{kFB + 0x70000, 0x10000, 0x10000}, Run{kFB + 0x100000, 0x100000, 0x100000}}, false, kObj);
    }

    // Refusals on the pad-only image (node pool count 4).
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        FakeMem base;
        base.InitMemory();
        std::map<uint64_t, uint64_t> node_of;
        BuildImage(&base, pre, kNoPeak, &node_of);
        BuildNodePool(&base, 4);
        const Prediction good = PredictOf(pre, 1, 0x30000, 0x20000);
        CHECK(good.path == Path::FlexHitNode);
        {
            FakeMem m = base;
            CHECK(ApplyNodeHit(m, kR, pre, good, 0x30000, 0x20000, 1, kObj) != 0);
        }
        {
            Prediction p = good;
            p.path = Path::FlexHitExact;
            RunRefuseNode("prediction says exact", base, pre, p, 0x30000, 0x20000, 1, kObj);
        }
        {
            Prediction p = good;
            p.va += 0x10000;
            RunRefuseNode("predicted VA differs from the aligned start", base, pre, p, 0x30000, 0x20000, 1, kObj);
        }
        RunRefuseNode("kind 2 with a flexible node prediction", base, pre, good, 0x30000, 0x20000, 2, kObj);
        RunRefuseNode("size not 64 KiB multiple", base, pre, good, 0x30001, 0x20000, 1, kObj);
        RunRefuseNode("obj unmapped (tag byte unreadable)", base, pre, good, 0x30000, 0x20000, 1, 0x20000000ull);
        {
            FakeMem m = base;
            m.Put(kR + 0x1f0, 0, 8); // head zero: the pool is corrupt
            RunRefuseNode("node pool head zero", m, pre, good, 0x30000, 0x20000, 1, kObj);
        }
        {
            FakeMem m = base;
            m.Put(kR + 0x200, 0, 4); // count 0 but the list has elements: the pop's count would go negative
            RunRefuseNode("node pool count negative after the pop", m, pre, good, 0x30000, 0x20000, 1, kObj);
        }
        {
            FakeMem m = base;
            const uint64_t E = m.U64(kR + 0x1f0);
            m.Put(E, E + 0x1000, 8); // the head's node pointer does not map back to E
            RunRefuseNode("node pool element corrupt", m, pre, good, 0x30000, 0x20000, 1, kObj);
        }
        {
            Pools bad = pre;
            bad.set[0].free_va_pages += 1;
            RunRefuseNode("snapshot free pages differ", base, bad, good, 0x30000, 0x20000, 1, kObj);
        }
        {
            Pools bad = pre;
            bad.set[0].free_va[0].base += 0x10000;
            RunRefuseNode("snapshot run base differs", base, bad, good, 0x30000, 0x20000, 1, kObj);
        }
        {
            FakeMem m = base;
            const uint64_t N1 = node_of.at(kFB + 0x10000);
            m.Put(N1 + 0x20 + 8, kR + 0x1234, 8); // prev link of the free element is wrong
            RunRefuseNode("corrupt prev link in the free list", m, pre, good, 0x30000, 0x20000, 1, kObj);
        }
        {
            FakeMem m = base;
            m.Put(kR + 0x10, 0, 8);
            RunRefuseNode("region head zero", m, pre, good, 0x30000, 0x20000, 1, kObj);
        }
        {
            FakeMem m = base;
            m.Put(kR + 0x20d908, kFB + 0x100000, 8); // the flexible base above the block: the tag index underflows
            RunRefuseNode("flexible base above the block", m, pre, good, 0x30000, 0x20000, 1, kObj);
        }
    }
    // Exact start forged as a node: ApplyNodeHit must refuse (ApplyExactHit's path).
    {
        Set s0 = MakeSet(0, {Run{kFB, 0x30000, 0x30000}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        FakeMem base;
        base.InitMemory();
        std::map<uint64_t, uint64_t> node_of;
        BuildImage(&base, pre, kNoPeak, &node_of);
        BuildNodePool(&base, 4);
        Prediction forged = PredictOf(pre, 1, 0x30000, 0x10000);
        CHECK(forged.path == Path::FlexHitExact);
        forged.path = Path::FlexHitNode;
        forged.slot = 0;
        forged.va = kFB;
        RunRefuseNode("exact start forged as a node", base, pre, forged, 0x30000, 0x10000, 1, kObj);
    }
    // Empty node pool: the pop cannot run.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        FakeMem base;
        base.InitMemory();
        std::map<uint64_t, uint64_t> node_of;
        BuildImage(&base, pre, kNoPeak, &node_of);
        BuildNodePool(&base, 0);
        const Prediction good = PredictOf(pre, 1, 0x30000, 0x20000);
        CHECK(good.path == Path::FlexHitNode);
        RunRefuseNode("empty node pool", base, pre, good, 0x30000, 0x20000, 1, kObj);
    }
    // A corrupt node pool whose only element is a node inside the root's header area (root+0xf8): the pop would store
    // into the set headers. The writer refuses before any write. (Nodes seeded at root+0x208 + k*0x70 are legitimate,
    // see the next case.)
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        FakeMem base;
        base.InitMemory();
        std::map<uint64_t, uint64_t> node_of;
        BuildImage(&base, pre, kNoPeak, &node_of);
        BuildNodePool(&base, 1);
        const uint64_t E = kR + 0x118;     // element: N = E - 0x20 = root+0xf8 (a header-area address)
        const uint64_t N = E - 0x20;
        base.Put(kR + 0x1f0, E, 8);        // head
        base.Put(kR + 0x1e8, E, 8);        // tail
        base.Put(E, N, 8);                 // [E] = N
        base.Put(E + 0x8, kR + 0x1e0, 8);  // prev = sentinel
        base.Put(E + 0x10, kR + 0x1e0, 8); // next = sentinel
        base.Put(kR + 0x200, 1, 4);
        const Prediction good = PredictOf(pre, 1, 0x30000, 0x20000);
        CHECK(good.path == Path::FlexHitNode);
        RunRefuseNode("node pool element in the header area", base, pre, good, 0x30000, 0x20000, 1, kObj);
    }
    // Seeded pool nodes inside the root struct, as the boot code lays them out (root+0x208 + k*0x70, linked in address
    // order): legitimate, so the pop is accepted and matches the reference byte for byte.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x40000, 0x40000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        FakeMem base;
        base.InitMemory();
        std::map<uint64_t, uint64_t> node_of;
        BuildImage(&base, pre, kNoPeak, &node_of);
        const int K = 4;
        std::vector<uint64_t> el;
        for (int i = 0; i < K; ++i) {
            const uint64_t N = kR + 0x208 + static_cast<uint64_t>(i) * 0x70;
            el.push_back(N + 0x20);
            base.Put(N + 0x20, N, 8);
        }
        for (int i = 0; i < K; ++i) {
            base.Put(el[i] + 0x8, i == 0 ? kR + 0x1e0 : el[i - 1], 8);
            base.Put(el[i] + 0x10, i + 1 == K ? kR + 0x1e0 : el[i + 1], 8);
        }
        base.Put(kR + 0x1f0, el[0], 8);
        base.Put(kR + 0x1e8, el[K - 1], 8);
        base.Put(kR + 0x200, K, 4);
        const Prediction good = PredictOf(pre, 1, 0x30000, 0x20000);
        CHECK(good.path == Path::FlexHitNode);
        FakeMem nat = base;
        const uint64_t rax = ApplyNodeHit(nat, kR, pre, good, 0x30000, 0x20000, 1, kObj, kOv);
        const auto writes = LastWriteSet();
        CHECK(rax != 0);
        FakeMem ref = base;
        Ref rr{&ref, kR, kObj, {}, false, false};
        const uint64_t rrax = RefFlexNode(rr, 0x30000, 0x20000);
        CHECK(!rr.refill && !rr.refused && rrax != 0);
        CHECK(rax == rrax);
        CHECK(SameImage(nat, ref));
        CHECK(SameLog(writes, rr.log));
        // Every root write lands in a node the writer read (the touched set), not elsewhere in the root struct.
        const std::vector<uint64_t> touched = SotcPcPoolNative::TouchedNodes();
        for (const auto& w : writes) {
            if (w.first < kR + 0x208 || w.first >= kR + 0x111908) {
                continue;
            }
            bool owned = false;
            for (uint64_t n : touched) {
                owned = owned || (w.first >= n && w.first + w.second <= n + 0x80);
            }
            CHECK(owned);
        }
        std::printf("[node] seeded pool nodes inside the root struct: %s\n", rax != 0 ? "accepted" : "BAD");
    }
    // Reproduction of the in-game kill (write 0x1000000310 = root+0x310): a pad and split pops N_0 (pad) and N_1 (split);
    // the split pop's next element is E_2 = root+0x308, so its prev-link store [E_2+8] lands at root+0x310 (N_2+0x28).
    // The glue's rule: a write is modelled when it lies in a node the writer touched (extent kNodeExtent).
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x10000, 0x80000, 0x80000}, Run{kFB + 0x100000, 0x100000, 0}}, 0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        FakeMem base;
        base.InitMemory();
        std::map<uint64_t, uint64_t> node_of;
        BuildImage(&base, pre, kNoPeak, &node_of);
        const int K = 8;
        std::vector<uint64_t> el;
        for (int i = 0; i < K; ++i) {
            const uint64_t N = kR + 0x208 + static_cast<uint64_t>(i) * 0x70;
            el.push_back(N + 0x20);
            base.Put(N + 0x20, N, 8);
        }
        for (int i = 0; i < K; ++i) {
            base.Put(el[i] + 0x8, i == 0 ? kR + 0x1e0 : el[i - 1], 8);
            base.Put(el[i] + 0x10, i + 1 == K ? kR + 0x1e0 : el[i + 1], 8);
        }
        base.Put(kR + 0x1f0, el[0], 8);
        base.Put(kR + 0x1e8, el[K - 1], 8);
        base.Put(kR + 0x200, K, 4);
        const Prediction good = PredictOf(pre, 1, 0x30000, 0x20000);
        CHECK(good.path == Path::FlexHitNode);
        FakeMem nat = base;
        const uint64_t rax = ApplyNodeHit(nat, kR, pre, good, 0x30000, 0x20000, 1, kObj, kOv);
        const auto writes = LastWriteSet();
        const std::vector<uint64_t> touched = SotcPcPoolNative::TouchedNodes();
        std::printf("[node] reproduction: pad and split, seeded K=%d: %s, %zu writes, %zu touched nodes\n", K,
                    rax != 0 ? "accepted" : "refused", writes.size(), touched.size());
        for (uint64_t n : touched) {
            std::printf("  touched node %llx (+0x20 = E %llx)\n", static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(n + 0x20));
        }
        CHECK(rax != 0);
        for (const auto& w : writes) {
            bool owned = false;
            for (uint64_t n : touched) {
                owned = owned || (w.first >= n && w.first + w.second <= n + kNodeExtentModel);
            }
            const bool header = w.first + w.second <= kR + 0x208 && w.first >= kR;
            const bool tags = w.first >= kR + 0x20d918 && w.first < kR + 0x22f000;
            const bool modelled = owned || header || tags || w.first >= kFB;
            if (!modelled) {
                std::printf("  NOT MODELLED: write 0x%llx (+%zu)\n", static_cast<unsigned long long>(w.first),
                            w.second);
            }
            CHECK(modelled);
        }
    }
}

} // namespace

int main() {
    // Case 1: flexible exact hit (kind 1, S0). R1 too small, R2 exact (backed == size), R3 large and unused.
    {
        Set s0 = MakeSet(0,
                         {Run{kFB, 0x20000, 0x20000}, Run{kFB + 0x40000, 0x30000, 0x30000},
                          Run{kFB + 0x100000, 0x100000, 0}},
                         0, 0, 0, 0);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunExact("flex exact, empty region list, obj set", pre, 1, 0x30000, 0x10000, kObj, Path::FlexHitExact,
                 kFB + 0x40000, kNoPeak, false);
    }
    // Case 2: flexible exact hit with a non-empty region list and low peaks (peak stores written), obj NULL.
    {
        Set s0 = MakeSet(0, {Run{kFB + 0x40000, 0x30000, 0x30000}}, 2, 0, 0x50000, 0x50000);
        Pools pre = MakePools(s0, Empty(1), Empty(2));
        RunExact("flex exact, 2 regions, peaks written, obj NULL", pre, 1, 0x30000, 0x10000, 0, Path::FlexHitExact,
                 kFB + 0x40000, 0, true);
    }
    // Case 3: primary S1 exact after S0 misses (S0 run too small). S1 run is larger than the request but its backed
    // size is not: tail dropped (0x40000 -> 0x30000). 1 MiB alignment, pad 0.
    {
        Set s0 = MakeSet(0, {Run{kFB, 0x10000, 0x10000}}, 0, 0, 0, 0);
        Set s1 = MakeSet(1, {Run{kOnion, 0x40000, 0x30000}}, 1, 0, 0x10000, 0x10000);
        Pools pre = MakePools(s0, s1, Empty(2));
        RunExact("primary S1 exact (kind 1, S0 misses), tail dropped", pre, 1, 0x30000, 0x10000, kObj,
                 Path::PrimaryHitExact, kOnion, kNoPeak, false);
    }
    // Case 4: primary S2 exact (kind 2), peaks written.
    {
        Set s2 = MakeSet(2, {Run{kGarlic, 0x80000, 0x30000}}, 0, 0, 0, 0);
        Pools pre = MakePools(Empty(0), Empty(1), s2);
        RunExact("primary S2 exact (kind 2), peaks written", pre, 2, 0x30000, 0x10000, kObj,
                 Path::PrimaryHitExact, kGarlic, 0, true);
    }
    // Case 5: primary S1 exact, S1 has a region list of 3 and its free list has a second, unused run.
    {
        Set s1 = MakeSet(1, {Run{kOnion + 0x400000, 0x20000, 0x20000}, Run{kOnion + 0x800000, 0x40000, 0x40000}},
                         3, 0x10, 0x40000, 0x40000);
        Pools pre = MakePools(Empty(0), s1, Empty(2));
        RunExact("primary S1, 3 regions, cached pages set, align 2 MiB", pre, 1, 0x40000, 0x200000, kObj,
                 Path::PrimaryHitExact, kOnion + 0x800000, kNoPeak, false);
    }

    // Refusals. A base image with one exact-hit candidate on S0 and a second candidate on S1.
    {
        Set s0 = MakeSet(0,
                         {Run{kFB, 0x20000, 0x20000}, Run{kFB + 0x40000, 0x30000, 0x30000}},
                         0, 0, 0, 0);
        Set s1 = MakeSet(1, {Run{kOnion, 0x40000, 0x30000}}, 1, 0, 0x10000, 0x10000);
        Pools pre = MakePools(s0, s1, Empty(2));
        std::map<uint64_t, uint64_t> node_of;
        FakeMem base;
        base.InitMemory();
        BuildImage(&base, pre, kNoPeak, &node_of);
        const Prediction good = PredictOf(pre, 1, 0x30000, 0x10000);
        CHECK(good.path == Path::FlexHitExact);

        // Sanity: the exact case itself applies.
        {
            FakeMem m = base;
            CHECK(ApplyExactHit(m, kR, pre, good, 0x30000, 0x10000, 1, kObj) != 0);
        }

        // R1: the snapshot's free VA pages differ from the guest.
        {
            Pools bad = pre;
            bad.set[0].free_va_pages += 1;
            RunRefuse("snapshot free pages differ", base, bad, good, 0x30000, 0x10000, 1, kObj);
        }
        // R2: the snapshot's list differs from the guest (base of the first run).
        {
            Pools bad = pre;
            bad.set[0].free_va[1].base += 0x10000;
            RunRefuse("snapshot run base differs", base, bad, good, 0x30000, 0x10000, 1, kObj);
        }
        // R3: the snapshot's region count differs from the guest.
        {
            Pools bad = pre;
            bad.set[0].region_count = 1;
            RunRefuse("snapshot region count differs", base, bad, good, 0x30000, 0x10000, 1, kObj);
        }
        // R4: the stored free-VA node count differs.
        {
            Pools bad = pre;
            bad.set[0].stored_free_count += 1;
            RunRefuse("stored free-VA count differs", base, bad, good, 0x30000, 0x10000, 1, kObj);
        }
        // R5: guest corruption: the prev link of the second run's element is wrong.
        {
            FakeMem m = base;
            const uint64_t N2 = node_of.at(kFB + 0x40000);
            m.Put(N2 + 0x20 + 8, kR + 0x1234, 8);
            RunRefuse("corrupt prev link in the free list", m, pre, good, 0x30000, 0x10000, 1, kObj);
        }
        // R6: obj not mapped (non-NULL, unreadable tag byte).
        RunRefuse("obj unmapped", base, pre, good, 0x30000, 0x10000, 1, 0x20000000ull);
        // R7: the prediction's VA differs from the first fit.
        {
            Prediction p = good;
            p.va += 0x10000;
            RunRefuse("predicted VA differs", base, pre, p, 0x30000, 0x10000, 1, kObj);
        }
        // R8: prediction slot and kind disagree (flex path with kind 2).
        {
            Prediction p = good;
            RunRefuse("flex path with kind 2", base, pre, p, 0x30000, 0x10000, 2, kObj);
        }
        // R9: the request size is not a multiple of 64 KiB.
        RunRefuse("size not 64 KiB multiple", base, pre, good, 0x30001, 0x10000, 1, kObj);
        // R10: the root's flexible base is above the run (tag index would underflow).
        {
            FakeMem m = base;
            m.Put(kR + 0x20d908, kFB + 0x100000, 8);
            RunRefuse("flexible base above the run", m, pre, good, 0x30000, 0x10000, 1, kObj);
        }
        // R11: the free pages of the set cover fewer pages than the request.
        {
            FakeMem m = base;
            m.Put(kR + 0x70, 2, 4);
            Pools bad = pre;
            bad.set[0].free_va_pages = 2;
            RunRefuse("S0 free pages below the request", m, bad, good, 0x30000, 0x10000, 1, kObj);
        }

        // R12: the first fit has a pad (split off the front): forged as an exact prediction.
        {
            Set s0b = MakeSet(0, {Run{kFB + 0x8000, 0x80000, 0x80000}}, 0, 0, 0, 0);
            Pools pre2 = MakePools(s0b, Empty(1), Empty(2));
            FakeMem m;
            m.InitMemory();
            std::map<uint64_t, uint64_t> no;
            BuildImage(&m, pre2, kNoPeak, &no);
            Prediction forged = PredictOf(pre2, 1, 0x30000, 0x10000);
            forged.path = Path::FlexHitExact;
            forged.va = kFB + 0x10000;
            RunRefuse("pad in the first fit (forged exact)", m, pre2, forged, 0x30000, 0x10000, 1, kObj);
            // And the real prediction is a node hit, not exact.
            CHECK(forged.path == Path::FlexHitExact);
            const Prediction real = PredictOf(pre2, 1, 0x30000, 0x10000);
            CHECK(real.path == Path::FlexHitNode);
            RunRefuse("pad in the first fit (real NodeHit prediction)", m, pre2, real, 0x30000, 0x10000, 1, kObj);
        }
        // R13: split (backed above the request) forged as exact.
        {
            Set s0c = MakeSet(0, {Run{kFB + 0x40000, 0x40000, 0x40000}}, 0, 0, 0, 0);
            Pools pre3 = MakePools(s0c, Empty(1), Empty(2));
            FakeMem m;
            m.InitMemory();
            std::map<uint64_t, uint64_t> no;
            BuildImage(&m, pre3, kNoPeak, &no);
            Prediction forged = PredictOf(pre3, 1, 0x30000, 0x10000);
            forged.path = Path::FlexHitExact;
            RunRefuse("split forged as exact", m, pre3, forged, 0x30000, 0x10000, 1, kObj);
        }
        // R14: primary claimed for kind 1 while S0 has an exact hit: the game would take S0.
        {
            Pools p14 = pre;
            Prediction forged = PredictOf(pre, 1, 0x30000, 0x10000);
            forged.path = Path::PrimaryHitExact;
            forged.slot = 1;
            forged.va = kOnion;
            RunRefuse("primary claimed while S0 hits", base, p14, forged, 0x30000, 0x10000, 1, kObj);
        }
        // R15: early reject: S1 and S2 cannot cover the request (forged primary on kind 2).
        {
            Set s2 = MakeSet(2, {Run{kGarlic, 0x10000, 0x10000}}, 0, 0, 0, 0);
            Pools p15 = MakePools(Empty(0), Empty(1), s2);
            FakeMem m;
            m.InitMemory();
            std::map<uint64_t, uint64_t> no;
            BuildImage(&m, p15, kNoPeak, &no);
            Prediction forged = PredictOf(p15, 2, 0x30000, 0x10000);
            forged.path = Path::PrimaryHitExact;
            forged.slot = 2;
            forged.va = kGarlic;
            RunRefuse("early reject (S1+S2 pages below request)", m, p15, forged, 0x30000, 0x10000, 2, kObj);
            const Prediction real = PredictOf(p15, 2, 0x30000, 0x10000);
            CHECK(real.outcome == Outcome::Null);
        }
        // R16: primary with a pad in the first fit (not 1 MiB aligned, forged exact).
        {
            Set s1p = MakeSet(1, {Run{kOnion + 0x80000, 0x200000, 0x200000}}, 0, 0, 0, 0);
            Pools p16 = MakePools(Empty(0), s1p, Empty(2));
            FakeMem m;
            m.InitMemory();
            std::map<uint64_t, uint64_t> no;
            BuildImage(&m, p16, kNoPeak, &no);
            const Prediction real = PredictOf(p16, 1, 0x30000, 0x10000);
            CHECK(real.path == Path::PrimaryHitNode);
            Prediction forged = real;
            forged.path = Path::PrimaryHitExact;
            forged.va = kOnion + 0x80000;
            RunRefuse("primary pad forged as exact", m, p16, forged, 0x30000, 0x10000, 1, kObj);
        }
        // R17: the prediction is not a predicted hit at all (fresh block path).
        {
            Prediction p = good;
            p.outcome = Outcome::Unmodelled;
            RunRefuse("prediction not Predicted", base, pre, p, 0x30000, 0x10000, 1, kObj);
        }
        // R18: the region list head is zero (no valid region list).
        {
            FakeMem m = base;
            m.Put(kR + 0x10, 0, 8);
            RunRefuse("region head zero", m, pre, good, 0x30000, 0x10000, 1, kObj);
        }
    }

    NodeCases();

    // Peak comparison in the other direction: peaks above, so no peak store, with obj set (checked in RunExact).
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
