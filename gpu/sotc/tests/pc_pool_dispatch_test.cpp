// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: standalone test of the native dispatch (gpu/sotc/pc_pool_native_dispatch.cpp). The fresh reserve and POOL_FIX
// paths never occur in the Lab route, so they are driven here through the same dispatch the glue calls (Apply), with a
// fake guest memory and a fake fresh host. Also checked: the modelled-memory check (InModelled) for the writes of those
// paths, its refill terms (SOTC_PC_POOL_REFILL: the bookkeeping qword and the refill block), the refusal routing, and
// PredictCall's refill gating.
//
// Build and run (scratch directory, not the repo). The writers are separate translation units, as in the other native
// tests (they define the same internal helper names):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_dispatch_test.cpp \
//     gpu/sotc/pc_pool_native.cpp gpu/sotc/pc_pool_native_nodes.cpp gpu/sotc/pc_pool_native_primary.cpp \
//     gpu/sotc/pc_pool_native_fresh.cpp gpu/sotc/pc_pool_native_reserve.cpp gpu/sotc/pc_pool_native_dispatch.cpp \
//     -o dispatch_test.exe
//   ./dispatch_test.exe   (prints PASS or FAIL; exit code 0 on PASS)
//
// The world is the one of pc_pool_native_fresh_test.cpp (TestReserveNative, TestPoolFixNative), rebuilt here; the
// expected results are the ones those tests check against a hand transcription of the guest image.
#define SOTC_PC_POOL_MODEL_ONLY
#include "../pc_pool.cpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pc_pool_native_dispatch.h"
#include "pc_pool_native_fresh.h"
#include "pc_pool_native_nodes.h"

namespace {

using SotcPcPool::Model::Outcome;
using SotcPcPool::Model::Path;
using SotcPcPool::Model::PhysMap;
using SotcPcPool::Model::PhysRun;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::Prediction;
using SotcPcPool::Model::Request;
using SotcPcPool::Model::Run;
using SotcPcPoolNativeFresh::BatchEntry;
using SotcPcPoolNativeFresh::FreshHost;
using SotcPcPoolNativeFresh::Refusal;
namespace D = SotcPcPoolDispatch;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Guest memory: a flat byte array.
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
        if (addr > bytes.size() || n > bytes.size() - addr) {
            return false;
        }
        std::memcpy(&bytes[static_cast<size_t>(addr)], in, n);
        return true;
    }
    void Put(uint64_t addr, uint64_t value, size_t n) { std::memcpy(&bytes[static_cast<size_t>(addr)], &value, n); }
    uint64_t Get(uint64_t addr, size_t n) const {
        uint64_t v = 0;
        std::memcpy(&v, &bytes[static_cast<size_t>(addr)], n);
        return v;
    }
    std::vector<uint8_t> bytes;
};

class FakeHost : public FreshHost {
public:
    uint64_t ReserveVirtual(uint64_t size, uint64_t align) override {
        ++reserve_calls;
        (void)size;
        (void)align;
        return reserve_result;
    }
    int32_t BatchMap(const BatchEntry* entries, size_t count) override {
        ++batch_calls;
        seen.assign(entries, entries + count);
        return 0;
    }
    uint64_t reserve_result = 0x2000000;
    int reserve_calls = 0;
    int batch_calls = 0;
    std::vector<BatchEntry> seen;
};

// Layout of the test guest (as pc_pool_native_fresh_test.cpp).
constexpr uint64_t kRoot = 0x10000;
constexpr uint64_t kMemSize = 8u << 20;
constexpr uint64_t kNodeBase = 0x400000;      // pool node 0 (the popped one), node k at kNodeBase + k * 0x100
constexpr uint64_t kObj = 0x500000;           // obj+0x1c is the owner tag
constexpr uint64_t kWordsS1 = 0x300000;
constexpr uint64_t kWordsS2 = 0x310000;
constexpr uint64_t kFreeNode1 = 0x700000;     // free-VA node of S1
constexpr uint64_t kVaFree = 0x40000000;      // VA of the free-VA node
constexpr uint64_t kVaBlock = 0x2000000;      // the VA the host returns
constexpr uint64_t kOverhead = 0x2b8000;      // stands for DAT_02dfca60 (mapped, outside every modelled range)
constexpr uint64_t kRefillBlock = 0x780000;   // a 64 KiB block the refill would carve (mapped, outside the rest)
constexpr uint64_t kOutside = 0x7f0000;       // mapped, nothing owns it

constexpr uint64_t kSentinel = kRoot + 0x1e0;
constexpr uint64_t kPoolHead = kRoot + 0x1f0;
constexpr uint64_t kPoolTail = kRoot + 0x1e8;
constexpr uint64_t kPoolCount = kRoot + 0x200;

uint64_t SetAddr(int index) { return kRoot + 0xa0 * static_cast<uint64_t>(index); }

// Five pool nodes on the list, the owner object.
void BuildPool(FlatMem* m, int count) {
    for (int k = 0; k < count; ++k) {
        const uint64_t node = kNodeBase + 0x100 * static_cast<uint64_t>(k);
        const uint64_t e = node + 0x20;
        const uint64_t prev = k == 0 ? kSentinel : kNodeBase + 0x100 * static_cast<uint64_t>(k - 1) + 0x20;
        const uint64_t next = k == count - 1 ? kSentinel : kNodeBase + 0x100 * static_cast<uint64_t>(k + 1) + 0x20;
        m->Put(e, node, 8);
        m->Put(e + 0x8, prev, 8);
        m->Put(e + 0x10, next, 8);
    }
    m->Put(kPoolHead, kNodeBase + 0x20, 8);
    m->Put(kPoolTail, kNodeBase + 0x100 * static_cast<uint64_t>(count - 1) + 0x20, 8);
    m->Put(kPoolCount, static_cast<uint64_t>(count), 4);
    m->Put(kObj + 0x1c, 0x5a, 1);
}

struct FreeSpec {
    uint64_t node;
    uint64_t base;
    uint64_t size;
    uint64_t backed;
};

struct SetSpec {
    int32_t cached = 0;
    uint64_t alloc = 0;
    uint64_t backed = 0;
    uint32_t cap = 128;
    std::vector<uint64_t> words;
};

// One set in guest memory and in the snapshot: header fields as the game uses them, bitmap words, and the free-VA list.
void BuildSet(FlatMem* m, Pools* pre, int index, const SetSpec& s, uint64_t words_addr,
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

// The world of the reserve and POOL_FIX scenarios: S1 has one free-VA node (2 MiB at kVaFree, backed 2 MiB), S2 is
// empty, five pool nodes, one owner.
struct World {
    FlatMem mem{kMemSize};
    Pools pre;
    World(int s1_cached) {
        BuildPool(&mem, 5);
        SetSpec s1;
        s1.cached = s1_cached;
        s1.words = {0, 0};
        SetSpec s2;
        s2.words = {0, 0};
        BuildSet(&mem, &pre, 0, SetSpec{}, 0, {});
        BuildSet(&mem, &pre, 1, s1, kWordsS1, {FreeSpec{kFreeNode1, kVaFree, 0x200000, 0x200000}});
        BuildSet(&mem, &pre, 2, s2, kWordsS2, {});
    }
};

Prediction ReservePrediction(Path path, uint64_t size) {
    Prediction p;
    p.outcome = Outcome::Predicted;
    p.path = path;
    p.slot = 1;
    p.fresh = true;
    p.va_align = 0x100000;
    p.size = size;
    PhysRun r;
    r.set = 1;
    r.first_page = path == Path::FreshReserve ? 16 : 32;
    r.pages = path == Path::FreshReserve ? 16 : 4;
    r.va = 0;
    r.protect_type = -1;
    p.runs.push_back(r);
    Run unm;
    unm.base = kVaFree;
    unm.size = 0x100000;
    p.unmapped.push_back(unm);
    return p;
}

Request FreshRequest(uint64_t size) {
    Request req;
    req.size = size;
    req.align = 0x10000;
    req.kind = 1;
    req.obj = kObj;
    return req;
}

// Every write the last Apply made must be inside the modelled memory of pre (the glue kills the session otherwise).
void CheckWritesModelled(const D::State& st, const Pools& pre, const std::string& what) {
    const auto writes = D::LastWriteSet(st);
    Check(!writes.empty(), what + ": the writer reported its writes");
    size_t outside = 0;
    for (const auto& [addr, n] : writes) {
        if (!D::InModelled(st, pre, addr, n)) {
            ++outside;
        }
    }
    Check(outside == 0, what + ": every write is in the modelled memory (" + std::to_string(outside) + " outside)");
}

// Scenario R through the dispatch: the reserve-map step (fresh reserve path).
void TestReserveRoute() {
    World w(0);
    PhysMap map;
    map.Map(kVaFree, 0x100000, 0x10);
    const Prediction pred = ReservePrediction(Path::FreshReserve, 0x100000);
    FakeHost host;
    D::State st;
    st.root = kRoot;
    st.map = &map;
    SotcPcPoolNativeFresh::ResetCounts();
    const uint64_t node = D::Apply(w.mem, host, &st, w.pre, pred, FreshRequest(0x100000));
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "reserve route: performed");
    Check(node == kNodeBase, "reserve route: returns the popped node");
    Check(st.writer == D::Writer::FreshReserve, "reserve route: the fresh reserve writer ran");
    Check(std::string(D::WriterName(st.writer)) == "fresh reserve", "reserve route: writer name");
    Check(host.reserve_calls == 1, "reserve route: one VA reservation");
    Check(host.batch_calls == 1 && host.seen.size() == 2, "reserve route: one flush, two entries");
    if (host.seen.size() == 2) {
        Check(host.seen[0].op == 1 && host.seen[0].va == kVaFree, "reserve route: entry 0 unmaps the consumed range");
        Check(host.seen[1].op == 0 && host.seen[1].va == kVaBlock, "reserve route: entry 1 maps the run");
    }
    Check(w.mem.Get(kPoolCount, 4) == 4, "reserve route: pool count after the pop");
    CheckWritesModelled(st, w.pre, "reserve route");
    Check(!D::InModelled(st, w.pre, kOutside, 8), "reserve route: an unowned address is outside the model");
    Check(!D::InModelled(st, w.pre, kOverhead, 8), "reserve route: the overhead qword is outside with refills off");
}

// Scenario P through the dispatch: the POOL_FIX path (S1 has cached pages but no free bit).
void TestPoolFixRoute() {
    World w(4);
    PhysMap map;
    map.Map(kVaFree, 0x40000, 0x20);
    const Prediction pred = ReservePrediction(Path::FreshPoolFix, 0x40000);
    FakeHost host;
    D::State st;
    st.root = kRoot;
    st.map = &map;
    SotcPcPoolNativeFresh::ResetCounts();
    const uint64_t node = D::Apply(w.mem, host, &st, w.pre, pred, FreshRequest(0x40000));
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::None, "pool fix route: performed");
    Check(node == kNodeBase, "pool fix route: returns the popped node");
    Check(st.writer == D::Writer::FreshPoolFix, "pool fix route: the POOL_FIX writer ran");
    Check(host.seen.size() == 2 && host.seen[1].op == 0 && host.seen[1].va == kVaBlock,
          "pool fix route: one flush, the run is mapped at the reserved VA");
    CheckWritesModelled(st, w.pre, "pool fix route");
}

// The map is missing: a reserve step is refused before the VA is reserved, nothing is written, and the refusal names
// the writer's reason.
void TestReserveRefusedWithoutMap() {
    World w(0);
    const Prediction pred = ReservePrediction(Path::FreshReserve, 0x100000);
    FlatMem before = w.mem;
    FakeHost host;
    D::State st;
    st.root = kRoot;
    st.map = nullptr;
    const uint64_t node = D::Apply(w.mem, host, &st, w.pre, pred, FreshRequest(0x100000));
    Check(node == 0, "no map: refused");
    Check(st.writer == D::Writer::FreshReserve, "no map: the refusing writer is recorded");
    Check(SotcPcPoolNativeFresh::LastRefusal() == Refusal::UnknownMap, "no map: refusal is UnknownMap");
    Check(std::string(D::RefusalReason(st)) == SotcPcPoolNativeFresh::RefusalName(Refusal::UnknownMap),
          "no map: the dispatch reports the writer's refusal name");
    Check(host.reserve_calls == 0 && host.batch_calls == 0, "no map: nothing reserved, nothing flushed");
    Check(w.mem.bytes == before.bytes, "no map: guest memory unchanged");
    Check(D::LastWriteSet(st).empty(), "no map: no writes reported");
}

// A path with no writer in the dispatch (Unmodelled) is refused with nothing written, and the state says so.
void TestUnroutedPath() {
    World w(0);
    Prediction pred = ReservePrediction(Path::FreshReserve, 0x100000);
    pred.path = Path::Unmodelled;
    FlatMem before = w.mem;
    FakeHost host;
    D::State st;
    st.root = kRoot;
    const uint64_t node = D::Apply(w.mem, host, &st, w.pre, pred, FreshRequest(0x100000));
    Check(node == 0, "unrouted: refused");
    Check(st.writer == D::Writer::None, "unrouted: no writer ran");
    Check(host.reserve_calls == 0 && host.batch_calls == 0, "unrouted: the host was not called");
    Check(w.mem.bytes == before.bytes, "unrouted: guest memory unchanged");
}

// The refill terms of InModelled: the bookkeeping qword and the refill block are owned only by a refilling call.
void TestRefillGate() {
    World w(0);
    Pools& pre = w.pre;
    D::State st;
    st.root = kRoot;
    // The overhead address is the image global: image base + 0x2dfca60 (not a root field).
    Check(SotcPcPoolNodes::OverheadAddr(0x100000000ull) == 0x100000000ull + 0x2dfca60ull,
          "gate: the overhead address is image base + 0x2dfca60");
    Check(SotcPcPoolNodes::kOverheadGlobal == 0x2dfca60, "gate: kOverheadGlobal is DAT_02dfca60");

    st.refill = false;
    st.overhead_addr = kOverhead;
    st.refill_block = kRefillBlock;
    Check(!D::InModelled(st, pre, kOverhead, 8), "gate: refills off: the overhead qword is refused");
    Check(!D::InModelled(st, pre, kRefillBlock + 0x100, 0x70), "gate: refills off: the refill block is refused");

    st.refill = true;
    Check(D::InModelled(st, pre, kOverhead, 8), "gate: refills on: the overhead qword is allowed");
    Check(D::InModelled(st, pre, kRefillBlock + 0x100, 0x70), "gate: refills on: a node of the refill block is allowed");
    Check(D::InModelled(st, pre, kRefillBlock + 0xffff - 7, 8), "gate: refills on: the block's last qword is allowed");
    Check(!D::InModelled(st, pre, kRefillBlock + 0x10000 - 4, 8), "gate: a write across the block end is refused");
    Check(!D::InModelled(st, pre, kOverhead + 4, 8), "gate: a write past the overhead qword is refused");

    st.refill_block = 0;
    Check(!D::InModelled(st, pre, kRefillBlock + 0x100, 0x70), "gate: no refill block named: refused");
    Check(D::InModelled(st, pre, kOverhead, 8), "gate: the overhead qword does not need a named block");

    st.overhead_addr = 0;
    Check(!D::InModelled(st, pre, 0, 8), "gate: overhead address 0 is not an allowance");
    Check(!D::InModelled(st, pre, kOutside, 8), "gate: an unowned address stays refused with refills on");
}

// PredictCall: with refills off it is Model::Predict; with them on, a path that pops no node is unchanged and names no
// refill block.
void TestPredictCall() {
    World w(0);
    PhysMap map;
    map.Map(kVaFree, 0x100000, 0x10);
    const Request req = FreshRequest(0x100000);
    Pools post_model;
    const Prediction model = SotcPcPool::Model::Predict(w.pre, req, &map, &post_model);
    Pools post_off;
    uint64_t blk = 0xdeadbeef;
    const Prediction off = D::PredictCall(w.pre, req, &map, &post_off, 5, false, &blk);
    Check(off.outcome == model.outcome && off.path == model.path, "predict: refills off is Model::Predict");
    Check(blk == 0, "predict: refills off names no refill block");
    Check(post_off.set[1].alloc_bytes == post_model.set[1].alloc_bytes, "predict: refills off, same post state");
    if (!SotcPcPoolNodes::PopsNode(model.path)) {
        Pools post_on;
        uint64_t blk_on = 0xdeadbeef;
        const Prediction on = D::PredictCall(w.pre, req, &map, &post_on, 3, true, &blk_on);
        Check(on.outcome == model.outcome && on.path == model.path, "predict: refills on, a non-node path is unchanged");
        Check(blk_on == 0, "predict: refills on, a non-node path names no refill block");
    }
}

} // namespace

int main() {
    TestReserveRoute();
    TestPoolFixRoute();
    TestReserveRefusedWithoutMap();
    TestUnroutedPath();
    TestRefillGate();
    TestPredictCall();
    std::printf("pc_pool_dispatch_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
