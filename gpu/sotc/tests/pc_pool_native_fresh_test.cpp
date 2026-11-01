// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: standalone test of pc_pool_native_fresh.cpp (with pc_pool_native_reserve.cpp under it). Build (no project
// build needed; the mutation check builds copies of the .cpp with one fault each and expects this test to fail):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -I gpu/sotc gpu/sotc/tests/pc_pool_native_fresh_test.cpp \
//       gpu/sotc/pc_pool_native_fresh.cpp gpu/sotc/pc_pool_native_reserve.cpp -o <scratch>/fresh_test.exe
// The PhysMap methods live in pc_pool.cpp, included here in its model-only form (the glue is not built).
// The expected memory images are transcribed by hand from the disassembly of eboot 0x19e00 (fresh path), 0x19660
// (pool_node_alloc) and 0x1a4b3-0x1a627 (page loop, batch ops), not from the code under test.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pc_pool_native_fresh.h"
#include "pc_pool_native_reserve.h"

#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"

namespace {

using SotcPcPool::Model::Pools;
using SotcPcPool::Model::PhysRun;
using SotcPcPool::Model::Prediction;
using SotcPcPool::Model::Run;
using SotcPcPoolNativeFresh::BatchEntry;
using SotcPcPoolNativeFresh::FreshHost;
using SotcPcPoolNativeFresh::Refusal;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Guest memory: a flat byte array, optional write failure after N writes.
class FlatMem : public SotcPcPoolNative::GuestMem {
public:
    explicit FlatMem(size_t n) : bytes(n, 0) {}
    bool Read(uint64_t addr, void* out, size_t n) override {
        if (addr > bytes.size() || n > bytes.size() - addr) {
            return false;
        }
        std::memcpy(out, &bytes[static_cast<size_t>(addr)], n);
        return true;
    }
    bool Write(uint64_t addr, const void* in, size_t n) override {
        if (writes_left == 0) {
            return false;
        }
        if (writes_left > 0) {
            --writes_left;
        }
        if (addr > bytes.size() || n > bytes.size() - addr) {
            return false;
        }
        std::memcpy(&bytes[static_cast<size_t>(addr)], in, n);
        return true;
    }
    void Put(uint64_t addr, uint64_t value, size_t n) {
        std::memcpy(&bytes[static_cast<size_t>(addr)], &value, n);
    }
    uint64_t Get(uint64_t addr, size_t n) const {
        uint64_t v = 0;
        std::memcpy(&v, &bytes[static_cast<size_t>(addr)], n);
        return v;
    }
    std::vector<uint8_t> bytes;
    int64_t writes_left = -1; ///< -1: unlimited
};

class FakeHost : public FreshHost {
public:
    uint64_t ReserveVirtual(uint64_t size, uint64_t align) override {
        ++reserve_calls;
        last_size = size;
        last_align = align;
        return reserve_result;
    }
    int32_t BatchMap(const BatchEntry* entries, size_t count) override {
        ++batch_calls;
        seen.assign(entries, entries + count);
        return batch_error;
    }
    uint64_t reserve_result = 0x2000000;
    int32_t batch_error = 0;
    int reserve_calls = 0;
    int batch_calls = 0;
    uint64_t last_size = 0;
    uint64_t last_align = 0;
    std::vector<BatchEntry> seen;
};

// Layout of the test guest (all addresses are guest addresses in FlatMem).
constexpr uint64_t kRoot = 0x10000;
constexpr uint64_t kMemSize = 8u << 20;
constexpr uint64_t kNodeBase = 0x400000; // node k at kNodeBase + k * 0x100
constexpr uint64_t kObj = 0x500000;      // obj+0x1c is the owner tag
constexpr uint64_t kOldElem = 0x600020;  // the old region-list head element of S1 and S2
constexpr uint64_t kWordsS1 = 0x300000;
constexpr uint64_t kWordsS2 = 0x310000;
constexpr uint64_t kFakeFree = 0x9000000; // a free-VA run in pre (only its emptiness is compared)

constexpr uint64_t kSentinel = kRoot + 0x1e0;
constexpr uint64_t kPoolHead = kRoot + 0x1f0;
constexpr uint64_t kPoolTail = kRoot + 0x1e8;
constexpr uint64_t kPoolCount = kRoot + 0x200;
constexpr uint64_t kBatchCount = kRoot + 0x2102f0;
constexpr uint64_t kBatchEntries = kRoot + 0x2102f8;
constexpr uint64_t kOwnerMiB = kRoot + 0x111908;

uint64_t SetAddr(int index) {
    return kRoot + 0xa0 * static_cast<uint64_t>(index);
}

struct SetSpec {
    int32_t region = 0;
    int32_t cached = 0;
    bool free_nonempty = false;
    uint64_t alloc = 0;
    uint64_t backed = 0;
    uint32_t cap = 128;
    std::vector<uint64_t> words;
};

// Writes one set into the guest and into the snapshot. Field offsets as the game uses them (0x19e00, 0x1abf0).
void BuildSet(FlatMem* m, Pools* pre, int index, const SetSpec& s, uint64_t words_addr) {
    const uint64_t a = SetAddr(index);
    m->Put(a + 0x78, static_cast<uint64_t>(index), 4);
    m->Put(a + 0x20, static_cast<uint64_t>(s.region), 4);
    m->Put(a + 0x74, static_cast<uint64_t>(s.cached), 4);
    m->Put(a + 0x38, s.free_nonempty ? a + 0x100 : a + 0x28, 8);
    m->Put(a + 0x50, words_addr, 8);
    m->Put(a + 0x58, s.words.size(), 4);
    m->Put(a + 0x60, s.cap, 4);
    m->Put(a + 0x80, s.alloc, 8);
    m->Put(a + 0x88, s.alloc, 8);
    m->Put(a + 0x90, s.backed, 8);
    m->Put(a + 0x98, s.backed, 8);
    for (size_t w = 0; w < s.words.size(); ++w) {
        m->Put(words_addr + 8 * w, s.words[w], 8);
    }
    // Region list head: the old element (its +0x10 is written by the link).
    m->Put(a + 0x8, kOldElem, 8);
    m->Put(kOldElem, kNodeBase, 8);
    m->Put(kOldElem + 0x10, 0x1234, 8);

    SotcPcPool::Model::Set& set = pre->set[index];
    set.id = index;
    set.has_bitmap = index != 0;
    set.region_count = s.region;
    set.cached_phys = s.cached;
    set.alloc_bytes = s.alloc;
    set.backed_bytes = s.backed;
    set.page_cap = s.cap;
    set.words = s.words;
    set.words_addr = words_addr;
    set.free_va.clear();
    if (s.free_nonempty) {
        set.free_va.push_back(Run{kFakeFree, 0x10000, 0x10000, 0});
    }
}

// The node pool: five nodes in a ring, head = node 0's element, count = 5.
void BuildPool(FlatMem* m, int count) {
    const uint64_t sentinel = kSentinel;
    for (int k = 0; k < count; ++k) {
        const uint64_t node = kNodeBase + 0x100 * static_cast<uint64_t>(k);
        const uint64_t e = node + 0x20;
        const uint64_t prev = k == 0 ? sentinel : kNodeBase + 0x100 * static_cast<uint64_t>(k - 1) + 0x20;
        const uint64_t next = k == count - 1 ? sentinel : kNodeBase + 0x100 * static_cast<uint64_t>(k + 1) + 0x20;
        m->Put(e, node, 8);
        m->Put(node + 0x28, prev, 8);
        m->Put(node + 0x30, next, 8);
        m->Put(node + 0x50, 0, 8);
    }
    m->Put(kPoolHead, kNodeBase + 0x20, 8);
    m->Put(kPoolTail, kNodeBase + 0x100 * static_cast<uint64_t>(count - 1) + 0x20, 8);
    m->Put(kPoolCount, static_cast<uint64_t>(count), 4);
}

// The owner object (tag at +0x1c).
void BuildObj(FlatMem* m) {
    m->Put(kObj + 0x1c, 0x5a, 1);
}

// The default scenario: S1 (kind 1 primary) and S2 (kind 2 primary), five pool nodes, one owner.
struct World {
    FlatMem mem{kMemSize};
    Pools pre;
    SetSpec s1;
    SetSpec s2;
    World() {
        BuildPool(&mem, 5);
        BuildObj(&mem);
    }
    void Build() {
        BuildSet(&mem, &pre, 1, s1, kWordsS1);
        BuildSet(&mem, &pre, 2, s2, kWordsS2);
        pre.set[0].id = 0;
        pre.set[0].has_bitmap = false;
        pre.set[0].words.clear();
    }
};

Prediction FreshPred(bool spill, int slot, uint64_t size, uint64_t align_1mib) {
    Prediction p;
    p.outcome = SotcPcPool::Model::Outcome::Predicted;
    p.path = spill ? SotcPcPool::Model::Path::FreshSpill : SotcPcPool::Model::Path::FreshPlain;
    p.slot = slot;
    p.va = 0;
    p.size = size;
    p.fresh = true;
    p.va_align = align_1mib;
    return p;
}

void AddRun(Prediction* p, int set, uint64_t first, uint64_t pages, int protect) {
    PhysRun r;
    r.set = set;
    r.first_page = first;
    r.pages = pages;
    r.va = 0;
    r.protect_type = protect;
    p->runs.push_back(r);
}

// ---- hand transcription helpers (only used to write the expected image) ----
void Exp(std::vector<uint8_t>* img, uint64_t addr, uint64_t value, size_t n) {
    std::memcpy(&(*img)[static_cast<size_t>(addr)], &value, n);
}

// Scenario A: plain, kind 2, one run of 2 pages from S2 bit 10 (bits 10..12 free, cached 3).
// Hand transcription from 0x19e00 (fresh path), 0x19660 (pop) and 0x1a4b3 (run + flush).
std::vector<uint8_t> ExpectPlain(const std::vector<uint8_t>& pre) {
    std::vector<uint8_t> e = pre;
    const uint64_t n1 = kNodeBase;             // node popped from the head
    const uint64_t e1 = n1 + 0x20;
    const uint64_t e2 = kNodeBase + 0x100 + 0x20;
    const uint64_t va = 0x2000000;
    const uint64_t size = 0x20000;
    const uint64_t s2 = SetAddr(2);
    // pool_node_alloc: unlink head, count - 1
    Exp(&e, n1 + 0x38, 0, 8);
    Exp(&e, n1 + 0x20, 0, 8);
    Exp(&e, kSentinel + 0x10, e2, 8);  // prev (the sentinel) .next = next element
    Exp(&e, e2 + 0x8, kSentinel, 8);   // next element .prev = sentinel
    Exp(&e, n1 + 0x30, e1, 8);
    Exp(&e, n1 + 0x28, e1, 8);
    Exp(&e, kPoolCount, 4, 4);
    // node fields
    Exp(&e, n1 + 0x68, 2, 1);
    Exp(&e, n1 + 0x58, size, 8);
    Exp(&e, n1 + 0x60, va, 8);
    Exp(&e, n1 + 0x69, 1, 1);
    Exp(&e, n1 + 0x40, kObj, 8);
    Exp(&e, n1 + 0x48, 0xffffffffu, 4);
    // owner tag: one 1 MiB unit (va >> 20 = 0x20)
    Exp(&e, kOwnerMiB + 0x20, 0x5a, 1);
    // region list: node element at the head of S2, old head points at it
    Exp(&e, n1 + 0x20, n1, 8);
    Exp(&e, n1 + 0x30, s2, 8);
    Exp(&e, n1 + 0x28, kOldElem, 8);
    Exp(&e, s2 + 0x8, n1 + 0x20, 8);
    Exp(&e, kOldElem + 0x10, n1 + 0x20, 8);
    Exp(&e, s2 + 0x20, 6, 4);
    Exp(&e, n1 + 0x38, s2, 8);
    // allocated bytes and peak (pre 0x4000000)
    Exp(&e, s2 + 0x80, 0x4020000, 8);
    Exp(&e, s2 + 0x88, 0x4020000, 8);
    // page loop: bits 10 and 11 cleared (word 0: 0x1c00 -> 0x1000), cached 3 -> 1
    Exp(&e, kWordsS2, 0x1000, 8);
    Exp(&e, s2 + 0x74, 1, 4);
    // batch op 0 at index 0: VA, phys 0x100000 + 10 * 0x10000, length, prot 0x33, type 0, op 0
    Exp(&e, kBatchEntries + 0, va, 8);
    Exp(&e, kBatchEntries + 8, 0x1a0000, 8);
    Exp(&e, kBatchEntries + 0x10, size, 8);
    Exp(&e, kBatchEntries + 0x18, 0x33, 1);
    Exp(&e, kBatchEntries + 0x19, 0, 1);
    Exp(&e, kBatchEntries + 0x1c, 0, 4);
    Exp(&e, kBatchCount, 1, 4);
    // node backed and set backed (pre 0x3000000) with peak
    Exp(&e, n1 + 0x50, size, 8);
    Exp(&e, s2 + 0x90, 0x3020000, 8);
    Exp(&e, s2 + 0x98, 0x3020000, 8);
    // vmem_batch_map: count reset
    Exp(&e, kBatchCount, 0, 4);
    return e;
}

SetSpec ScenarioA_s2() {
    SetSpec s;
    s.region = 5;
    s.cached = 3;
    s.alloc = 0x4000000;
    s.backed = 0x3000000;
    s.cap = 128;
    s.words = {0x1c00, 0}; // bits 10, 11, 12 free
    return s;
}

// Builds scenario A's world and prediction.
void MakeA(World* w, Prediction* pred) {
    w->s1 = SetSpec{};
    w->s1.region = 1;
    w->s1.words = {0, 0};
    w->s2 = ScenarioA_s2();
    w->Build();
    *pred = FreshPred(false, 2, 0x20000, 0x100000);
    AddRun(pred, 2, 10, 2, -1);
}

void TestPlain() {
    World w;
    Prediction pred;
    MakeA(&w, &pred);
    const std::vector<uint8_t> expected = ExpectPlain(w.mem.bytes);
    FakeHost host;
    const uint64_t node = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                 kObj, 0x100000);
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "plain: performed");
    Check(node == kNodeBase, "plain: returns the popped node");
    Check(host.reserve_calls == 1 && host.last_size == 0x20000 && host.last_align == 0x100000,
          "plain: reserve(size, max(1 MiB, align)) once");
    Check(host.batch_calls == 1 && host.seen.size() == 1, "plain: one flush with one entry");
    Check(SotcPcPoolNativeFresh::LastFlushError() == 0, "plain: flush ok");
    size_t diff = 0;
    while (diff < expected.size() && expected[diff] == w.mem.bytes[diff]) {
        ++diff;
    }
    Check(diff == expected.size(), "plain: guest image equals the hand transcription (first diff at " +
                                       std::to_string(diff) + ")");
    Check(!SotcPcPoolNativeFresh::LastWriteSet().empty(), "plain: write set recorded");
}

// Scenario B: spill, kind 1. S1 (P) has no cached pages and no free VA, so the loop takes 3 pages from S2 (F).
void MakeB(World* w, Prediction* pred) {
    w->s1.region = 4;
    w->s1.alloc = 0x1000000;
    w->s1.backed = 0x1000000;
    w->s1.cap = 128;
    w->s1.words = {0, 0};
    w->s2.region = 5;
    w->s2.cached = 4;
    w->s2.alloc = 0x2000000;
    w->s2.backed = 0x2000000;
    w->s2.cap = 128;
    w->s2.words = {0xf, 0};
    w->Build();
    *pred = FreshPred(true, 1, 0x30000, 0x100000);
    AddRun(pred, 2, 0, 3, 0);
}

std::vector<uint8_t> ExpectSpill(const std::vector<uint8_t>& pre) {
    std::vector<uint8_t> e = pre;
    const uint64_t n1 = kNodeBase;
    const uint64_t e1 = n1 + 0x20;
    const uint64_t e2 = kNodeBase + 0x100 + 0x20;
    const uint64_t va = 0x3000000;
    const uint64_t size = 0x30000;
    const uint64_t s1 = SetAddr(1);
    const uint64_t s2 = SetAddr(2);
    Exp(&e, n1 + 0x38, 0, 8);
    Exp(&e, n1 + 0x20, 0, 8);
    Exp(&e, kSentinel + 0x10, e2, 8);
    Exp(&e, e2 + 0x8, kSentinel, 8);
    Exp(&e, n1 + 0x30, e1, 8);
    Exp(&e, n1 + 0x28, e1, 8);
    Exp(&e, kPoolCount, 4, 4);
    Exp(&e, n1 + 0x68, 1, 1); // pool id of the primary (S1)
    Exp(&e, n1 + 0x58, size, 8);
    Exp(&e, n1 + 0x60, va, 8);
    Exp(&e, n1 + 0x69, 1, 1);
    Exp(&e, n1 + 0x40, 0, 8); // no owner
    Exp(&e, n1 + 0x48, 0xffffffffu, 4);
    Exp(&e, kOwnerMiB + 0x30, 0, 1); // owner tag 0, one unit (va >> 20 = 0x30)
    Exp(&e, n1 + 0x20, n1, 8);
    Exp(&e, n1 + 0x30, s1, 8);
    Exp(&e, n1 + 0x28, kOldElem, 8);
    Exp(&e, s1 + 0x8, n1 + 0x20, 8);
    Exp(&e, kOldElem + 0x10, n1 + 0x20, 8);
    Exp(&e, s1 + 0x20, 5, 4);
    Exp(&e, n1 + 0x38, s1, 8);
    Exp(&e, s1 + 0x80, 0x1030000, 8);
    Exp(&e, s1 + 0x88, 0x1030000, 8);
    // page loop: F = S2, bits 0..2 cleared (0xf -> 0x8), cached 4 -> 1; op 0 then op 4 (type 0: F is S2)
    Exp(&e, kWordsS2, 0x8, 8);
    Exp(&e, s2 + 0x74, 1, 4);
    Exp(&e, kBatchEntries + 0, va, 8);
    Exp(&e, kBatchEntries + 8, 0x100000, 8);
    Exp(&e, kBatchEntries + 0x10, size, 8);
    Exp(&e, kBatchEntries + 0x18, 0x33, 1);
    Exp(&e, kBatchEntries + 0x19, 0, 1);
    Exp(&e, kBatchEntries + 0x1c, 0, 4);
    Exp(&e, kBatchEntries + 0x20 + 0, va, 8);
    Exp(&e, kBatchEntries + 0x20 + 8, 0x100000, 8);
    Exp(&e, kBatchEntries + 0x20 + 0x10, size, 8);
    Exp(&e, kBatchEntries + 0x20 + 0x18, 0x33, 1);
    Exp(&e, kBatchEntries + 0x20 + 0x19, 0, 1);
    Exp(&e, kBatchEntries + 0x20 + 0x1c, 4, 4);
    Exp(&e, kBatchCount, 0, 4);
    Exp(&e, n1 + 0x50, size, 8);
    Exp(&e, s2 + 0x90, 0x2030000, 8);
    Exp(&e, s2 + 0x98, 0x2030000, 8);
    return e;
}

void TestSpill() {
    World w;
    Prediction pred;
    MakeB(&w, &pred);
    const std::vector<uint8_t> expected = ExpectSpill(w.mem.bytes);
    FakeHost host;
    host.reserve_result = 0x3000000;
    const uint64_t node = SotcPcPoolNativeFresh::ApplyFreshSpill(w.mem, host, kRoot, w.pre, pred, 0x30000, 0x10000, 1,
                                                                 0, 0x100000);
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "spill: performed");
    Check(node == kNodeBase, "spill: returns the popped node");
    Check(host.seen.size() == 2 && host.seen[1].op == 4 && host.seen[1].type == 0,
          "spill: op 4 entry follows the op 0 entry, type 0 for S2");
    size_t diff = 0;
    while (diff < expected.size() && expected[diff] == w.mem.bytes[diff]) {
        ++diff;
    }
    Check(diff == expected.size(), "spill: guest image equals the hand transcription (first diff at " +
                                       std::to_string(diff) + ")");
}

// Scenario C: two runs, the second from the spill set. S1 cached 2 at bits 5,6; S2 cached 3 at bits 0..2.
void MakeC(World* w, Prediction* pred) {
    w->s1.region = 4;
    w->s1.cached = 2;
    w->s1.alloc = 0x1000000;
    w->s1.backed = 0x1000000;
    w->s1.cap = 128;
    w->s1.words = {0x60, 0};
    w->s2.region = 5;
    w->s2.cached = 3;
    w->s2.alloc = 0x2000000;
    w->s2.backed = 0x2000000;
    w->s2.cap = 128;
    w->s2.words = {0x7, 0};
    w->Build();
    *pred = FreshPred(true, 1, 0x40000, 0x100000);
    AddRun(pred, 1, 5, 2, -1);
    AddRun(pred, 2, 0, 2, 0);
}

void TestTwoRuns() {
    World w;
    Prediction pred;
    MakeC(&w, &pred);
    FakeHost host;
    host.reserve_result = 0x3000000;
    const uint64_t node = SotcPcPoolNativeFresh::ApplyFreshSpill(w.mem, host, kRoot, w.pre, pred, 0x40000, 0x10000, 1,
                                                                 0, 0x100000);
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "two runs: performed");
    Check(node == kNodeBase, "two runs: node");
    Check(host.seen.size() == 3, "two runs: three ops (op 0, op 0, op 4)");
    if (host.seen.size() == 3) {
        Check(host.seen[0].va == 0x3000000 && host.seen[0].offset == 0x150000 && host.seen[0].length == 0x20000 &&
                  host.seen[0].op == 0,
              "two runs: run 1 op 0 at bit 5 (phys 0x150000)");
        Check(host.seen[1].va == 0x3020000 && host.seen[1].offset == 0x100000 && host.seen[1].op == 0,
              "two runs: run 2 op 0 at VA+0x20000, bit 0");
        Check(host.seen[2].va == 0x3020000 && host.seen[2].op == 4 && host.seen[2].type == 0,
              "two runs: run 2 op 4 type 0");
    }
    Check(w.mem.Get(kWordsS1, 8) == 0, "two runs: S1 bits 5,6 cleared");
    Check(w.mem.Get(kWordsS2, 8) == 0x4, "two runs: S2 bits 0,1 cleared");
    Check(w.mem.Get(SetAddr(1) + 0x74, 4) == 0 && w.mem.Get(SetAddr(2) + 0x74, 4) == 1, "two runs: cached counts");
    Check(w.mem.Get(kNodeBase + 0x50, 8) == 0x40000, "two runs: node backed");
    Check(w.mem.Get(SetAddr(1) + 0x90, 8) == 0x1020000 && w.mem.Get(SetAddr(2) + 0x90, 8) == 0x2020000,
          "two runs: backed bytes charged to the set each run came from");
    Check(w.mem.Get(kBatchCount, 4) == 0, "two runs: count reset");
}

// ---- refusals: each must write nothing (the image is unchanged) unless the write set says otherwise ----
bool SameImage(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a == b;
}

void ExpectRefusal(const char* name, Refusal want, uint64_t got, const FlatMem& mem,
                   const std::vector<uint8_t>& before, bool expect_no_writes) {
    const Refusal have = SotcPcPoolNativeFresh::LastRefusal();
    Check(got == 0 && have == want,
          std::string(name) + ": refused as " + SotcPcPoolNativeFresh::RefusalName(want) + " (got " +
              SotcPcPoolNativeFresh::RefusalName(have) + ", node " + std::to_string(got) + ")");
    if (expect_no_writes) {
        Check(SameImage(before, mem.bytes) && SotcPcPoolNativeFresh::LastWriteSet().empty(),
              std::string(name) + ": nothing written");
    }
}

void TestRefusals() {
    // size not a multiple of 64 KiB
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x18000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("size", Refusal::Size, n, w.mem, before, true);
        Check(host.reserve_calls == 0, "size: no reservation");
    }
    // the route: the plain entry point given the spill path
    {
        World w;
        Prediction pred;
        MakeB(&w, &pred);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x30000, 0x10000, 1,
                                                                  0, 0x100000);
        ExpectRefusal("route plain-on-spill", Refusal::Route, n, w.mem, before, true);
    }
    // the spill entry point given a plain prediction
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshSpill(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("route spill-on-plain", Refusal::Route, n, w.mem, before, true);
    }
    // the prediction is not a fresh block
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        pred.fresh = false;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("prediction not fresh", Refusal::Prediction, n, w.mem, before, true);
    }
    // the predicted alignment is not the reserve alignment
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        pred.va_align = 0x200000;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("alignment", Refusal::Prediction, n, w.mem, before, true);
    }
    // the predicted runs differ from the plan
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        pred.runs[0].first_page = 11;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("runs differ", Refusal::Prediction, n, w.mem, before, true);
    }
    // node pool empty
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.mem.Put(kPoolHead, kSentinel, 8);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("node pool empty", Refusal::NodePool, n, w.mem, before, true);
    }
    // node pool at 3: the pop leaves 2, the game refills through 0x19e00
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.mem.Put(kPoolCount, 3, 4);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("node pool refill", Refusal::NodePool, n, w.mem, before, true);
    }
    // S2 has no cached pages but a free-VA node: the game runs vmem_query_reserve_map
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.s2.cached = 0;
        w.s2.words = {0, 0};
        w.Build();
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        // no PhysMap passed: the reserve-map step is native, but it needs the shadow map (UnknownMap, nothing written)
        ExpectRefusal("reserve-map without a map", Refusal::UnknownMap, n, w.mem, before, true);
    }
    // POOL_FIX: cached count positive but no free bit
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.s2.words = {0, 0};
        w.Build();
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("pool fix without a map", Refusal::UnknownMap, n, w.mem, before, true);
    }
    // batch queue not empty at entry
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.mem.Put(kBatchCount, 2, 4);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("batch pending", Refusal::Batch, n, w.mem, before, true);
    }
    // the guest bitmap differs from the snapshot
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.mem.Put(kWordsS2, 0x1c01, 8);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("bitmap mismatch", Refusal::GuestMismatch, n, w.mem, before, true);
    }
    // the cached count differs from the snapshot
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.mem.Put(SetAddr(2) + 0x74, 4, 4);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("cached mismatch", Refusal::GuestMismatch, n, w.mem, before, true);
    }
    // the owner object is not mapped
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  0x7ffffff0, 0x100000);
        ExpectRefusal("owner unmapped", Refusal::GuestUnmapped, n, w.mem, before, true);
    }
    // the host has no VA: nothing written, no flush
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        FakeHost host;
        host.reserve_result = 0;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("host reserve fails", Refusal::HostReserve, n, w.mem, before, true);
        Check(host.batch_calls == 0, "host reserve fails: no flush");
    }
    // the host VA is not aligned: refused, nothing written (the reservation is leaked)
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        FakeHost host;
        host.reserve_result = 0x2000100;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        ExpectRefusal("host VA unaligned", Refusal::HostVa, n, w.mem, before, true);
    }
    // a write fails in the middle: the earlier writes stay, the write set says so
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        w.mem.writes_left = 5;
        FakeHost host;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        Check(n == 0 && SotcPcPoolNativeFresh::LastRefusal() == Refusal::GuestWrite,
              "write failure: refused as guest-write");
        Check(!SotcPcPoolNativeFresh::LastWriteSet().empty(), "write failure: the write set is not empty");
        Check(host.batch_calls == 0, "write failure: no flush");
    }
    // the flush fails: the node is still returned, the error is kept for the caller
    {
        World w;
        Prediction pred;
        MakeA(&w, &pred);
        FakeHost host;
        host.batch_error = 5;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x20000, 0x10000, 2,
                                                                  kObj, 0x100000);
        Check(n == kNodeBase && SotcPcPoolNativeFresh::LastRefusal() == Refusal::None,
              "flush error: node returned");
        Check(SotcPcPoolNativeFresh::LastFlushError() == 5, "flush error: kept for the caller");
    }
}

// ---- native reserve-map and POOL_FIX (ApplyFreshReserve / ApplyFreshPoolFix) ----
namespace NR = SotcPcPoolNativeReserve;
using SotcPcPool::Model::Path;
using SotcPcPool::Model::PhysMap;

struct FreeSpec {
    uint64_t node;
    uint64_t base;
    uint64_t size;
    uint64_t backed;
};

constexpr uint64_t kFreeNode1 = 0x700000; // free-VA node of S1 (address order first)
constexpr uint64_t kFreeNode2 = 0x710000; // second free-VA node of S1
constexpr uint64_t kVaFree = 0x40000000;  // VA of node 1
constexpr uint64_t kVaFree2 = 0x41000000; // VA of node 2
constexpr uint64_t kVaBlock = 0x2000000;  // the VA the host returns
constexpr uint64_t kPhys = 0x100000;

// A set with an empty region list (S+8 and S+0x10 are the sentinel) and the given free-VA list (address order).
void BuildSetX(FlatMem* m, Pools* pre, int index, const SetSpec& s, uint64_t words_addr,
               const std::vector<FreeSpec>& frees) {
    const uint64_t a = SetAddr(index);
    const uint64_t F = a + 0x28;
    m->Put(a + 0x78, static_cast<uint64_t>(index), 4);
    m->Put(a + 0x20, 0, 4);
    m->Put(a + 0x8, a, 8);
    m->Put(a + 0x10, a, 8);
    m->Put(a + 0x74, static_cast<uint64_t>(s.cached), 4);
    m->Put(a + 0x50, words_addr, 8);
    m->Put(a + 0x58, s.words.size(), 4);
    m->Put(a + 0x60, s.cap, 4);
    m->Put(a + 0x80, s.alloc, 8);
    m->Put(a + 0x88, s.alloc, 8);
    m->Put(a + 0x90, s.backed, 8);
    m->Put(a + 0x98, s.backed, 8);
    for (size_t w = 0; w < s.words.size(); ++w) {
        m->Put(words_addr + 8 * w, s.words[w], 8);
    }
    uint64_t pages = 0;
    for (size_t i = 0; i < frees.size(); ++i) {
        const FreeSpec& f = frees[i];
        const uint64_t E = f.node + 0x20;
        const uint64_t prev = i == 0 ? F : frees[i - 1].node + 0x20;
        const uint64_t next = i + 1 == frees.size() ? F : frees[i + 1].node + 0x20;
        m->Put(E, f.node, 8);
        m->Put(E + 0x8, prev, 8);
        m->Put(E + 0x10, next, 8);
        m->Put(f.node + 0x50, f.backed, 8);
        m->Put(f.node + 0x58, f.size, 8);
        m->Put(f.node + 0x60, f.base, 8);
        m->Put(f.node + 0x68, static_cast<uint64_t>(index), 1);
        m->Put(f.node + 0x69, 0, 1);
        pages += f.size / 0x10000;
    }
    m->Put(a + 0x38, frees.empty() ? F : frees[0].node + 0x20, 8);
    m->Put(a + 0x30, frees.empty() ? F : frees.back().node + 0x20, 8);
    m->Put(a + 0x48, frees.size(), 4);
    m->Put(a + 0x70, pages, 4);

    SotcPcPool::Model::Set& set = pre->set[index];
    set = SotcPcPool::Model::Set();
    set.id = index;
    set.has_bitmap = index != 0;
    set.region_count = 0;
    set.cached_phys = s.cached;
    set.alloc_bytes = s.alloc;
    set.backed_bytes = s.backed;
    set.page_cap = s.cap;
    set.words = s.words;
    set.words_addr = words_addr;
    set.free_va_pages = static_cast<int32_t>(pages);
    set.stored_free_count = static_cast<int32_t>(frees.size());
    for (const FreeSpec& f : frees) {
        set.free_va.push_back(Run{f.base, f.size, f.backed, f.node});
    }
}

struct WorldX {
    FlatMem mem{kMemSize};
    Pools pre;
    WorldX() {
        BuildPool(&mem, 5);
        BuildObj(&mem);
    }
    void Build(const SetSpec& s1, const SetSpec& s2, const std::vector<FreeSpec>& f1,
               const std::vector<FreeSpec>& f2) {
        BuildSetX(&mem, &pre, 0, SetSpec{}, 0, {});
        BuildSetX(&mem, &pre, 1, s1, kWordsS1, f1);
        BuildSetX(&mem, &pre, 2, s2, kWordsS2, f2);
    }
};

// The fresh prefix of 0x19e00 for kind 1 (S1 = P) with one block at kVaBlock: pool pop, node fields, owner tag,
// region link (S1 was empty), allocated bytes. Hand transcription; the same as the plain scenario's prefix.
void ExpectPrefixS1(std::vector<uint8_t>* e, uint64_t size) {
    const uint64_t n1 = kNodeBase;
    const uint64_t e1 = n1 + 0x20;
    const uint64_t e2 = kNodeBase + 0x100 + 0x20;
    const uint64_t s1 = SetAddr(1);
    Exp(e, n1 + 0x38, 0, 8);
    Exp(e, n1 + 0x20, 0, 8);
    Exp(e, kSentinel + 0x10, e2, 8);
    Exp(e, e2 + 0x8, kSentinel, 8);
    Exp(e, n1 + 0x30, e1, 8);
    Exp(e, n1 + 0x28, e1, 8);
    Exp(e, kPoolCount, 4, 4);
    Exp(e, n1 + 0x68, 1, 1);
    Exp(e, n1 + 0x58, size, 8);
    Exp(e, n1 + 0x60, kVaBlock, 8);
    Exp(e, n1 + 0x69, 1, 1);
    Exp(e, n1 + 0x40, kObj, 8);
    Exp(e, n1 + 0x48, 0xffffffffu, 4);
    Exp(e, kOwnerMiB + 0x20, 0x5a, 1); // one 1 MiB unit (VA 0x2000000 >> 20 = 0x20)
    Exp(e, n1 + 0x20, n1, 8);
    Exp(e, n1 + 0x30, s1, 8);
    Exp(e, n1 + 0x28, s1, 8);
    Exp(e, s1 + 0x8, e1, 8);
    Exp(e, s1 + 0x10, e1, 8);
    Exp(e, s1 + 0x20, 1, 4);
    Exp(e, n1 + 0x38, s1, 8);
    Exp(e, s1 + 0x80, size, 8);
    Exp(e, s1 + 0x88, size, 8);
}

// Scenario R: kind 1, S1 has no cached pages. Its free-VA node N1 (2 MiB at kVaFree) has 16 mapped pages at bits
// 16..31. Request 1 MiB. The loop: reserve-map on N1 (1 MiB, not used up), scan (bits 16..31), one run.
std::vector<uint8_t> ExpectReserveR(const std::vector<uint8_t>& pre) {
    std::vector<uint8_t> e = pre;
    const uint64_t n1 = kNodeBase;
    const uint64_t s1 = SetAddr(1);
    const uint64_t size = 0x100000;
    ExpectPrefixS1(&e, size);
    // vmem_query_reserve_map flag 1 (0x1abf0) on N1: bits 16..31 taken, N1 backed 2 MiB - 1 MiB, unmap op 1 of 1 MiB,
    // free VA pages 32 - 16, N1 keeps its tail (pool id, size, base, in-use 0, owner 0, -1)
    Exp(&e, kWordsS1, 0xffff0000, 8);
    Exp(&e, s1 + 0x74, 16, 4);
    Exp(&e, kFreeNode1 + 0x50, 0x100000, 8);
    Exp(&e, kBatchEntries + 0x00, kVaFree, 8);
    Exp(&e, kBatchEntries + 0x08, 0, 8);
    Exp(&e, kBatchEntries + 0x10, 0x100000, 8);
    Exp(&e, kBatchEntries + 0x18, 0, 1);
    Exp(&e, kBatchEntries + 0x19, 0, 1);
    Exp(&e, kBatchEntries + 0x1c, 1, 4);
    Exp(&e, kBatchCount, 1, 4);
    Exp(&e, s1 + 0x70, 16, 4);
    Exp(&e, kFreeNode1 + 0x68, 1, 1);
    Exp(&e, kFreeNode1 + 0x58, 0x100000, 8);
    Exp(&e, kFreeNode1 + 0x60, kVaFree + 0x100000, 8);
    Exp(&e, kFreeNode1 + 0x69, 0, 1);
    Exp(&e, kFreeNode1 + 0x40, 0, 8);
    Exp(&e, kFreeNode1 + 0x48, 0xffffffffu, 4);
    // scan: run of 16 pages from bit 16 (phys 0x100000 + 16 * 64 KiB = 0x200000) at VA 0x2000000
    Exp(&e, kWordsS1, 0, 8);
    Exp(&e, s1 + 0x74, 0, 4);
    Exp(&e, kBatchEntries + 0x20, kVaBlock, 8);
    Exp(&e, kBatchEntries + 0x28, 0x200000, 8);
    Exp(&e, kBatchEntries + 0x30, size, 8);
    Exp(&e, kBatchEntries + 0x38, 0x33, 1);
    Exp(&e, kBatchEntries + 0x39, 0, 1);
    Exp(&e, kBatchEntries + 0x3c, 0, 4);
    Exp(&e, kBatchCount, 2, 4);
    Exp(&e, n1 + 0x50, size, 8);
    Exp(&e, s1 + 0x90, size, 8);
    Exp(&e, s1 + 0x98, size, 8);
    Exp(&e, kBatchCount, 0, 4); // vmem_batch_map resets the count
    return e;
}

// Scenario P: kind 1, S1 has cached 4 but no free bit (POOL_FIX). N1 as in R, 4 mapped pages at bits 32..35.
// Request 4 pages. The patch zeroes cached, the reserve-map step takes the 4 bits, the scan runs them.
std::vector<uint8_t> ExpectPoolFixP(const std::vector<uint8_t>& pre) {
    std::vector<uint8_t> e = pre;
    const uint64_t n1 = kNodeBase;
    const uint64_t s1 = SetAddr(1);
    const uint64_t size = 0x40000;
    ExpectPrefixS1(&e, size);
    // POOL_FIX: cached := 0 (mov dword [r14+0x74], 0), then the reserve-map step on N1 (1 MiB, not used up)
    Exp(&e, s1 + 0x74, 0, 4);
    Exp(&e, kWordsS1, 0xf00000000ull, 8);
    Exp(&e, s1 + 0x74, 4, 4);
    Exp(&e, kFreeNode1 + 0x50, 0x1c0000, 8); // 2 MiB - 4 pages
    Exp(&e, kBatchEntries + 0x00, kVaFree, 8);
    Exp(&e, kBatchEntries + 0x08, 0, 8);
    Exp(&e, kBatchEntries + 0x10, 0x100000, 8);
    Exp(&e, kBatchEntries + 0x18, 0, 1);
    Exp(&e, kBatchEntries + 0x19, 0, 1);
    Exp(&e, kBatchEntries + 0x1c, 1, 4);
    Exp(&e, kBatchCount, 1, 4);
    Exp(&e, s1 + 0x70, 16, 4);
    Exp(&e, kFreeNode1 + 0x68, 1, 1);
    Exp(&e, kFreeNode1 + 0x58, 0x100000, 8);
    Exp(&e, kFreeNode1 + 0x60, kVaFree + 0x100000, 8);
    Exp(&e, kFreeNode1 + 0x69, 0, 1);
    Exp(&e, kFreeNode1 + 0x40, 0, 8);
    Exp(&e, kFreeNode1 + 0x48, 0xffffffffu, 4);
    // scan again: bits 32..35 (phys 0x100000 + 32 * 64 KiB = 0x300000), 4 pages at VA 0x2000000
    Exp(&e, kWordsS1, 0, 8);
    Exp(&e, s1 + 0x74, 0, 4);
    Exp(&e, kBatchEntries + 0x20, kVaBlock, 8);
    Exp(&e, kBatchEntries + 0x28, 0x300000, 8);
    Exp(&e, kBatchEntries + 0x30, size, 8);
    Exp(&e, kBatchEntries + 0x38, 0x33, 1);
    Exp(&e, kBatchEntries + 0x39, 0, 1);
    Exp(&e, kBatchEntries + 0x3c, 0, 4);
    Exp(&e, kBatchCount, 2, 4);
    Exp(&e, n1 + 0x50, size, 8);
    Exp(&e, s1 + 0x90, size, 8);
    Exp(&e, s1 + 0x98, size, 8);
    Exp(&e, kBatchCount, 0, 4);
    return e;
}

void TestReserveNative() {
    WorldX w;
    SetSpec s1;
    s1.cap = 128;
    s1.words = {0, 0};
    SetSpec s2;
    s2.cap = 128;
    s2.words = {0, 0};
    w.Build(s1, s2, {FreeSpec{kFreeNode1, kVaFree, 0x200000, 0x200000}}, {});
    PhysMap map;
    map.Map(kVaFree, 0x100000, 0x10);
    Prediction pred;
    pred.outcome = SotcPcPool::Model::Outcome::Predicted;
    pred.path = Path::FreshReserve;
    pred.slot = 1;
    pred.fresh = true;
    pred.va_align = 0x100000;
    pred.size = 0x100000;
    AddRun(&pred, 1, 16, 16, -1);
    Run unm;
    unm.base = kVaFree;
    unm.size = 0x100000;
    pred.unmapped.push_back(unm);
    const std::vector<uint8_t> expected = ExpectReserveR(w.mem.bytes);
    FakeHost host;
    host.reserve_result = kVaBlock;
    NR::ResetCounters();
    SotcPcPoolNativeFresh::ResetCounts();
    const uint64_t node = SotcPcPoolNativeFresh::ApplyFreshReserve(w.mem, host, kRoot, w.pre, pred, 0x100000, 0x10000, 1,
                                                                   kObj, kPhys, &map);
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "reserve: performed");
    Check(node == kNodeBase, "reserve: returns the popped node");
    Check(host.batch_calls == 1 && host.seen.size() == 2, "reserve: one flush with two entries");
    if (host.seen.size() == 2) {
        Check(host.seen[0].op == 1 && host.seen[0].va == kVaFree && host.seen[0].length == 0x100000,
              "reserve: entry 0 is the unmap of the consumed range (op 1, before the run)");
        Check(host.seen[1].op == 0 && host.seen[1].va == kVaBlock && host.seen[1].offset == 0x200000,
              "reserve: entry 1 is the run's map (op 0)");
    }
    Check(NR::Counters().reserve_ok == 1, "reserve: the step is counted once (dry run restored)");
    Check(SotcPcPoolNativeFresh::PerformedCount() == 1, "reserve: performed count");
    size_t diff = 0;
    while (diff < expected.size() && expected[diff] == w.mem.bytes[diff]) {
        ++diff;
    }
    Check(diff == expected.size(), "reserve: guest image equals the hand transcription (first diff at " +
                                       std::to_string(diff) + ")");
}

void TestPoolFixNative() {
    WorldX w;
    SetSpec s1;
    s1.cached = 4;
    s1.cap = 128;
    s1.words = {0, 0};
    SetSpec s2;
    s2.cap = 128;
    s2.words = {0, 0};
    w.Build(s1, s2, {FreeSpec{kFreeNode1, kVaFree, 0x200000, 0x200000}}, {});
    PhysMap map;
    map.Map(kVaFree, 0x40000, 0x20);
    Prediction pred;
    pred.outcome = SotcPcPool::Model::Outcome::Predicted;
    pred.path = Path::FreshPoolFix;
    pred.slot = 1;
    pred.fresh = true;
    pred.va_align = 0x100000;
    pred.size = 0x40000;
    AddRun(&pred, 1, 32, 4, -1);
    Run unm;
    unm.base = kVaFree;
    unm.size = 0x100000;
    pred.unmapped.push_back(unm);
    const std::vector<uint8_t> expected = ExpectPoolFixP(w.mem.bytes);
    FakeHost host;
    host.reserve_result = kVaBlock;
    NR::ResetCounters();
    SotcPcPoolNativeFresh::ResetCounts();
    const uint64_t node = SotcPcPoolNativeFresh::ApplyFreshPoolFix(w.mem, host, kRoot, w.pre, pred, 0x40000, 0x10000, 1,
                                                                   kObj, kPhys, &map);
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "pool fix: performed");
    Check(node == kNodeBase, "pool fix: returns the popped node");
    Check(NR::Counters().poolfix_ok == 1 && NR::Counters().reserve_ok == 0, "pool fix: counted as POOL_FIX");
    size_t diff = 0;
    while (diff < expected.size() && expected[diff] == w.mem.bytes[diff]) {
        ++diff;
    }
    Check(diff == expected.size(), "pool fix: guest image equals the hand transcription (first diff at " +
                                       std::to_string(diff) + ")");
}

// Scenario S (sticky node): S1 has two free-VA nodes. N1 (4 MiB at kVaFree, 64 backed pages) and M (2 MiB at
// kVaFree2, 32 backed pages). Request 48 pages. Mapped: 16 pages at N1 bits 16..31, the next 16 pages of N1
// (VA kVaFree + 3 MiB) at bits 64..79, 16 pages of M at bits 80..95. The first reserve-map step takes N1's first 3 MiB
// (partly consumed, rax = N1). The second step reuses N1 (rax) instead of searching, which would pick M (32 pages
// equal the remainder). N1 is then used up (rax = 0), so the third step searches and takes M.
void MakeSticky(WorldX* w, PhysMap* map, Prediction* pred) {
    SetSpec s1;
    s1.cap = 128;
    s1.words = {0, 0};
    SetSpec s2;
    s2.cap = 128;
    s2.words = {0, 0};
    w->Build(s1, s2,
             {FreeSpec{kFreeNode1, kVaFree, 0x400000, 0x400000}, FreeSpec{kFreeNode2, kVaFree2, 0x200000, 0x200000}},
             {});
    map->Map(kVaFree, 0x100000, 0x10);
    map->Map(kVaFree + 0x300000, 0x100000, 0x40);
    map->Map(kVaFree2, 0x100000, 0x50);
    pred->outcome = SotcPcPool::Model::Outcome::Predicted;
    pred->path = Path::FreshReserve;
    pred->slot = 1;
    pred->fresh = true;
    pred->va_align = 0x100000;
    pred->size = 0x300000;
    AddRun(pred, 1, 16, 16, -1);
    AddRun(pred, 1, 64, 16, -1);
    AddRun(pred, 1, 80, 16, -1);
    Run a;
    a.base = kVaFree;
    a.size = 0x300000;
    Run b;
    b.base = kVaFree + 0x300000;
    b.size = 0x100000;
    Run c;
    c.base = kVaFree2;
    c.size = 0x100000;
    pred->unmapped = {a, b, c};
}

void TestStickyNode() {
    WorldX w;
    PhysMap map;
    Prediction pred;
    MakeSticky(&w, &map, &pred);
    FakeHost host;
    host.reserve_result = kVaBlock;
    NR::ResetCounters();
    SotcPcPoolNativeFresh::ResetCounts();
    const uint64_t node = SotcPcPoolNativeFresh::ApplyFreshReserve(w.mem, host, kRoot, w.pre, pred, 0x300000, 0x10000,
                                                                   1, kObj, kPhys, &map);
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "sticky: performed");
    Check(node == kNodeBase, "sticky: node");
    Check(host.seen.size() == 6, "sticky: six entries (op 1, op 0 per step)");
    if (host.seen.size() == 6) {
        Check(host.seen[0].op == 1 && host.seen[0].va == kVaFree && host.seen[0].length == 0x300000,
              "sticky: step 1 unmaps 3 MiB of N1");
        Check(host.seen[2].op == 1 && host.seen[2].va == kVaFree + 0x300000 && host.seen[2].length == 0x100000,
              "sticky: step 2 takes N1's 1 MiB tail (rax, not the search)");
        Check(host.seen[4].op == 1 && host.seen[4].va == kVaFree2 && host.seen[4].length == 0x100000,
              "sticky: step 3 searches and takes M");
    }
    Check(w.mem.Get(kFreeNode1 + 0x60, 8) == 0, "sticky: N1 used up (base zeroed, pushed on the node pool)");
    Check(w.mem.Get(kFreeNode2 + 0x60, 8) == kVaFree2 + 0x100000, "sticky: M keeps its tail");
    Check(w.mem.Get(SetAddr(1) + 0x70, 4) == 16, "sticky: free VA pages 96 - 48 - 16 - 16");
    Check(w.mem.Get(kPoolCount, 4) == 5, "sticky: node pool back to 5 (4 after the pop, +1 for N1)");
    Check(NR::Counters().reserve_ok == 3, "sticky: three reserve-map steps counted");
    Check(w.mem.Get(kBatchCount, 4) == 0, "sticky: count reset");
}

void TestNativeRefusals() {
    // POOL_FIX with no free-VA node: the latent crash (NextSet), nothing written, counted
    {
        WorldX w;
        SetSpec s1;
        s1.cached = 4;
        s1.cap = 128;
        s1.words = {0, 0};
        SetSpec s2;
        s2.cap = 128;
        s2.words = {0, 0};
        w.Build(s1, s2, {}, {});
        PhysMap map;
        Prediction pred;
        pred.outcome = SotcPcPool::Model::Outcome::Predicted;
        pred.path = Path::FreshPoolFix;
        pred.fresh = true;
        pred.va_align = 0x100000;
        pred.size = 0x40000;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        SotcPcPoolNativeFresh::ResetCounts();
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPoolFix(w.mem, host, kRoot, w.pre, pred, 0x40000, 0x10000,
                                                                    1, kObj, kPhys, &map);
        ExpectRefusal("pool fix with no node", Refusal::NextSet, n, w.mem, before, true);
        Check(SotcPcPoolNativeFresh::RefusalCount(Refusal::NextSet) == 1, "pool fix with no node: counted");
    }
    // a reserve step with no node on either list: the game dereferences null (NoNode)
    {
        WorldX w;
        SetSpec s1;
        s1.cap = 128;
        s1.words = {0, 0};
        SetSpec s2;
        s2.cap = 128;
        s2.words = {0, 0};
        w.Build(s1, s2, {}, {});
        PhysMap map;
        Prediction pred;
        pred.outcome = SotcPcPool::Model::Outcome::Predicted;
        pred.path = Path::FreshReserve;
        pred.fresh = true;
        pred.va_align = 0x100000;
        pred.size = 0x100000;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshReserve(w.mem, host, kRoot, w.pre, pred, 0x100000, 0x10000,
                                                                    1, kObj, kPhys, &map);
        ExpectRefusal("reserve with no node", Refusal::NoNode, n, w.mem, before, true);
    }
    // the reserve step needs the map: none passed
    {
        WorldX w;
        PhysMap map;
        Prediction pred;
        MakeSticky(&w, &map, &pred);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshReserve(w.mem, host, kRoot, w.pre, pred, 0x300000, 0x10000,
                                                                    1, kObj, kPhys, nullptr);
        ExpectRefusal("reserve without a map", Refusal::UnknownMap, n, w.mem, before, true);
    }
    // a set bit at or past the page cap (the game's out-of-bounds path, not the patch)
    {
        WorldX w;
        SetSpec s1;
        s1.cached = 1;
        s1.cap = 2;
        s1.words = {0x8};
        SetSpec s2;
        s2.cap = 128;
        s2.words = {0, 0};
        w.Build(s1, s2, {}, {});
        Prediction pred;
        pred.outcome = SotcPcPool::Model::Outcome::Predicted;
        pred.path = Path::FreshPlain;
        pred.fresh = true;
        pred.va_align = 0x100000;
        pred.size = 0x10000;
        AddRun(&pred, 1, 3, 1, -1);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x10000, 0x10000, 1,
                                                                  kObj, kPhys, nullptr);
        ExpectRefusal("bit past the cap", Refusal::PageCap, n, w.mem, before, true);
    }
    // the prediction says plain, the state needs the reserve step (dry run route)
    {
        WorldX w;
        SetSpec s1;
        s1.cap = 128;
        s1.words = {0, 0};
        SetSpec s2;
        s2.cap = 128;
        s2.words = {0, 0};
        w.Build(s1, s2, {FreeSpec{kFreeNode1, kVaFree, 0x200000, 0x200000}}, {});
        PhysMap map;
        map.Map(kVaFree, 0x100000, 0x10);
        Prediction pred;
        pred.outcome = SotcPcPool::Model::Outcome::Predicted;
        pred.path = Path::FreshPlain;
        pred.fresh = true;
        pred.va_align = 0x100000;
        pred.size = 0x100000;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x100000, 0x10000, 1,
                                                                  kObj, kPhys, &map);
        ExpectRefusal("route plain-for-reserve", Refusal::Route, n, w.mem, before, true);
    }
    // the prediction names the reserve path for an entry point of another path
    {
        WorldX w;
        PhysMap map;
        Prediction pred;
        MakeSticky(&w, &map, &pred);
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshSpill(w.mem, host, kRoot, w.pre, pred, 0x300000, 0x10000,
                                                                  1, kObj, kPhys, &map);
        ExpectRefusal("route reserve-for-spill", Refusal::Route, n, w.mem, before, true);
    }
    // the prediction's reserve steps differ from the plan (the model's search would pick M, not N1)
    {
        WorldX w;
        PhysMap map;
        Prediction pred;
        MakeSticky(&w, &map, &pred);
        pred.unmapped[1].base = kVaFree2;
        pred.unmapped[1].size = 0x100000;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshReserve(w.mem, host, kRoot, w.pre, pred, 0x300000, 0x10000,
                                                                    1, kObj, kPhys, &map);
        ExpectRefusal("prediction searches, game is sticky", Refusal::Prediction, n, w.mem, before, true);
        Check(host.reserve_calls == 0, "prediction searches, game is sticky: no VA reserved");
    }
    // the host has no VA: nothing written, and the reserve steps are not counted
    {
        WorldX w;
        SetSpec s1;
        s1.cap = 128;
        s1.words = {0, 0};
        SetSpec s2;
        s2.cap = 128;
        s2.words = {0, 0};
        w.Build(s1, s2, {FreeSpec{kFreeNode1, kVaFree, 0x200000, 0x200000}}, {});
        PhysMap map;
        map.Map(kVaFree, 0x100000, 0x10);
        Prediction pred;
        pred.outcome = SotcPcPool::Model::Outcome::Predicted;
        pred.path = Path::FreshReserve;
        pred.fresh = true;
        pred.va_align = 0x100000;
        pred.size = 0x100000;
        AddRun(&pred, 1, 16, 16, -1);
        Run unm;
        unm.base = kVaFree;
        unm.size = 0x100000;
        pred.unmapped.push_back(unm);
        FakeHost host;
        host.reserve_result = 0;
        NR::ResetCounters();
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshReserve(w.mem, host, kRoot, w.pre, pred, 0x100000, 0x10000,
                                                                    1, kObj, kPhys, &map);
        ExpectRefusal("reserve: host fails", Refusal::HostReserve, n, w.mem, before, true);
        Check(NR::Counters().reserve_ok == 0, "reserve: host fails, the dry run's count is restored");
        Check(host.batch_calls == 0, "reserve: host fails, no flush");
    }
    // a write fails in the commit after the reserve step: the earlier writes stay
    {
        WorldX w;
        SetSpec s1;
        s1.cap = 128;
        s1.words = {0, 0};
        SetSpec s2;
        s2.cap = 128;
        s2.words = {0, 0};
        w.Build(s1, s2, {FreeSpec{kFreeNode1, kVaFree, 0x200000, 0x200000}}, {});
        PhysMap map;
        map.Map(kVaFree, 0x100000, 0x10);
        Prediction pred;
        pred.outcome = SotcPcPool::Model::Outcome::Predicted;
        pred.path = Path::FreshReserve;
        pred.fresh = true;
        pred.va_align = 0x100000;
        pred.size = 0x100000;
        AddRun(&pred, 1, 16, 16, -1);
        Run unm;
        unm.base = kVaFree;
        unm.size = 0x100000;
        pred.unmapped.push_back(unm);
        w.mem.writes_left = 4;
        FakeHost host;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshReserve(w.mem, host, kRoot, w.pre, pred, 0x100000, 0x10000,
                                                                    1, kObj, kPhys, &map);
        Check(n == 0 && SotcPcPoolNativeFresh::LastRefusal() == Refusal::GuestWrite,
              "reserve: write failure refused as guest-write");
        Check(SotcPcPoolNativeFresh::LastWriteSet().size() == 4, "reserve: the four writes that landed are listed");
        Check(host.batch_calls == 0, "reserve: write failure, no flush");
    }
    // an entry point for the path the call does not take (plain for a reserve prediction): refused before any work
    {
        WorldX w;
        PhysMap map;
        Prediction pred;
        MakeSticky(&w, &map, &pred);
        pred.path = Path::FreshPlain;
        FakeHost host;
        const std::vector<uint8_t> before = w.mem.bytes;
        const uint64_t n = SotcPcPoolNativeFresh::ApplyFreshPlain(w.mem, host, kRoot, w.pre, pred, 0x300000, 0x10000,
                                                                  1, kObj, kPhys, &map);
        ExpectRefusal("route entry point", Refusal::Route, n, w.mem, before, true);
    }
}

} // namespace

int main() {
    TestPlain();
    TestSpill();
    TestTwoRuns();
    TestRefusals();
    TestReserveNative();
    TestPoolFixNative();
    TestStickyNode();
    TestNativeRefusals();
    std::printf("pc_pool_native_fresh_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
